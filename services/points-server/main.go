package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"strings"
	"time"

	amqp "github.com/rabbitmq/amqp091-go"
	_ "github.com/go-sql-driver/mysql"
	pb "gateway-grpc/gen/rpc"
	"github.com/redis/go-redis/v9"
	"google.golang.org/grpc"
	"google.golang.org/grpc/health"
	"google.golang.org/grpc/health/grpc_health_v1"
	"google.golang.org/grpc/reflection"
)

type pointsServer struct {
	pb.UnimplementedPointsServiceServer
	db     *sql.DB
	rdb    *redis.Client
	amqpCh *amqp.Channel
}

func initDB(dsn string) *sql.DB {
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		slog.Error("mysql open failed", "error", err)
		os.Exit(1)
	}
	db.SetMaxOpenConns(10)
	db.SetMaxIdleConns(3)
	db.SetConnMaxLifetime(5 * time.Minute)
	if err := db.Ping(); err != nil {
		slog.Error("mysql ping failed", "error", err)
		os.Exit(1)
	}
	migrate(db)
	return db
}

func initAMQP(addr string) *amqp.Channel {
	conn, err := amqp.Dial(addr)
	if err != nil {
		slog.Error("rabbitmq dial failed", "error", err)
		os.Exit(1)
	}
	ch, err := conn.Channel()
	if err != nil {
		slog.Error("rabbitmq channel failed", "error", err)
		os.Exit(1)
	}
	ch.ExchangeDeclare("rpc.events", "topic", true, false, false, false, nil)
	q, _ := ch.QueueDeclare("pts.earn", true, false, false, false, nil)
	ch.QueueBind(q.Name, "sheet.created", "rpc.events", false, nil)
	ch.QueueBind(q.Name, "file.uploaded", "rpc.events", false, nil)
	ch.QueueBind(q.Name, "order.created", "rpc.events", false, nil)
	return ch
}

func migrate(db *sql.DB) {
	ddls := []string{
		`CREATE TABLE IF NOT EXISTS point_accounts (
			user_id BIGINT PRIMARY KEY,
			balance BIGINT NOT NULL DEFAULT 0,
			total_earned BIGINT NOT NULL DEFAULT 0,
			streak_count INT NOT NULL DEFAULT 0,
			last_login_date DATE,
			updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
		)`,
		`ALTER TABLE point_accounts ADD COLUMN streak_count INT NOT NULL DEFAULT 0`,
		`ALTER TABLE point_accounts ADD COLUMN last_login_date DATE`,
		`CREATE TABLE IF NOT EXISTS point_transactions (
			id BIGINT AUTO_INCREMENT PRIMARY KEY,
			user_id BIGINT NOT NULL,
			type VARCHAR(16) NOT NULL,
			amount BIGINT NOT NULL,
			reason VARCHAR(64) NOT NULL,
			ref_id VARCHAR(128) DEFAULT '',
			created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			INDEX idx_user_time (user_id, created_at)
		)`,
	}
	for _, ddl := range ddls {
		if _, err := db.Exec(ddl); err != nil {
			slog.Warn("migrate warning", "error", err)
		}
	}
}

// ---- GetBalance (Redis cache -> MySQL fallback) ----

func (s *pointsServer) GetBalance(ctx context.Context, req *pb.GetBalanceRequest) (*pb.BalanceResponse, error) {
	key := fmt.Sprintf("pts:%d", req.UserId)
	bal, err := s.rdb.HGet(ctx, key, "balance").Int64()
	if err == nil {
		total, _ := s.rdb.HGet(ctx, key, "total_earned").Int64()
		return &pb.BalanceResponse{Success: true, UserId: req.UserId, Balance: bal, TotalEarned: total}, nil
	}
	var balance, totalEarned int64
	s.db.QueryRow("SELECT balance, total_earned FROM point_accounts WHERE user_id=?", req.UserId).
		Scan(&balance, &totalEarned)
	s.rdb.HSet(ctx, key, "balance", balance, "total_earned", totalEarned)
	s.rdb.Expire(ctx, key, 5*time.Minute)
	return &pb.BalanceResponse{Success: true, UserId: req.UserId, Balance: balance, TotalEarned: totalEarned}, nil
}

func (s *pointsServer) GetTransactions(ctx context.Context, req *pb.GetTransactionsRequest) (*pb.TransactionsResponse, error) {
	limit := req.Limit
	if limit <= 0 || limit > 100 {
		limit = 20
	}
	rows, err := s.db.Query("SELECT id, type, amount, reason, created_at FROM point_transactions WHERE user_id=? ORDER BY created_at DESC LIMIT ?", req.UserId, limit)
	if err != nil {
		return &pb.TransactionsResponse{Success: true}, nil
	}
	defer rows.Close()
	resp := &pb.TransactionsResponse{Success: true}
	for rows.Next() {
		var tx pb.Transaction
		rows.Scan(&tx.Id, &tx.Type, &tx.Amount, &tx.Reason, &tx.CreatedAt)
		resp.Transactions = append(resp.Transactions, &tx)
	}
	return resp, nil
}

// ---- Earn: MySQL transaction -> Redis cache ----

func (s *pointsServer) Earn(ctx context.Context, req *pb.EarnRequest) (*pb.BalanceResponse, error) {
	if req.IdempotencyKey != "" {
		ok, _ := s.rdb.SetNX(ctx, "pts:dup:"+req.IdempotencyKey, "1", 5*time.Minute).Result()
		if !ok {
			bal := s.balanceFromMySQL(ctx, req.UserId)
			return &pb.BalanceResponse{Success: false, Balance: bal, Error: "duplicate"}, nil
		}
	}
	tx, _ := s.db.Begin()
	defer tx.Rollback()
	tx.Exec("INSERT INTO point_accounts (user_id, balance, total_earned) VALUES (?,?,?) ON DUPLICATE KEY UPDATE balance=balance+?, total_earned=total_earned+?",
		req.UserId, req.Amount, req.Amount, req.Amount, req.Amount)
	res, err := tx.Exec("INSERT INTO point_transactions (user_id, type, amount, reason) VALUES (?,?,?,?)",
		req.UserId, "earn", req.Amount, req.Reason)
	if err != nil {
		return &pb.BalanceResponse{Success: false, Error: "db error"}, nil
	}
	txID, _ := res.LastInsertId()
	tx.Commit()

	key := fmt.Sprintf("pts:%d", req.UserId)
	pipe := s.rdb.Pipeline()
	pipe.HIncrBy(ctx, key, "balance", req.Amount)
	pipe.HIncrBy(ctx, key, "total_earned", req.Amount)
	pipe.Expire(ctx, key, 5*time.Minute)
	cmds, _ := pipe.Exec(ctx)
	balance := cmds[0].(*redis.IntCmd).Val()
	total := cmds[1].(*redis.IntCmd).Val()

	b, _ := json.Marshal(map[string]interface{}{"id": txID, "type": "earn", "amount": req.Amount, "reason": req.Reason, "created_at": time.Now().Format(time.RFC3339)})
	s.rdb.LPush(ctx, fmt.Sprintf("pts:tx:%d", req.UserId), string(b))
	s.rdb.LTrim(ctx, fmt.Sprintf("pts:tx:%d", req.UserId), 0, 99)

	return &pb.BalanceResponse{Success: true, UserId: req.UserId, Balance: balance, TotalEarned: total}, nil
}

// ---- Deduct: MySQL FOR UPDATE -> transaction -> Redis ----

func (s *pointsServer) Deduct(ctx context.Context, req *pb.DeductRequest) (*pb.BalanceResponse, error) {
	tx, _ := s.db.Begin()
	defer tx.Rollback()
	var balance int64
	err := tx.QueryRow("SELECT balance FROM point_accounts WHERE user_id=? FOR UPDATE", req.UserId).Scan(&balance)
	if err != nil {
		return &pb.BalanceResponse{Success: false, Error: "account not found"}, nil
	}
	if balance < req.Amount {
		return &pb.BalanceResponse{Success: false, Balance: balance, Error: "insufficient balance"}, nil
	}
	tx.Exec("UPDATE point_accounts SET balance=balance-? WHERE user_id=?", req.Amount, req.UserId)
	tx.Exec("INSERT INTO point_transactions (user_id, type, amount, reason, ref_id) VALUES (?,?,?,?,?)",
		req.UserId, "deduct", -req.Amount, req.Reason, req.RefId)
	tx.Commit()

	newBal := balance - req.Amount
	key := fmt.Sprintf("pts:%d", req.UserId)
	s.rdb.HIncrBy(ctx, key, "balance", -req.Amount)
	s.rdb.Expire(ctx, key, 5*time.Minute)
	return &pb.BalanceResponse{Success: true, UserId: req.UserId, Balance: newBal}, nil
}

// ---- Leaderboard ----

func (s *pointsServer) GetLeaderboard(ctx context.Context, req *pb.LeaderboardRequest) (*pb.LeaderboardResponse, error) {
	limit := int(req.Limit)
	if limit <= 0 || limit > 100 {
		limit = 20
	}
	rows, err := s.db.Query("SELECT user_id, total_earned FROM point_accounts ORDER BY total_earned DESC LIMIT ?", limit)
	if err != nil {
		return &pb.LeaderboardResponse{Success: true}, nil
	}
	defer rows.Close()
	resp := &pb.LeaderboardResponse{Success: true}
	for rows.Next() {
		var uid, total int64
		rows.Scan(&uid, &total)
		resp.Entries = append(resp.Entries, &pb.LeaderboardEntry{UserId: uid, TotalEarned: total})
	}
	return resp, nil
}

// ---- Streak (Redis only) ----

func (s *pointsServer) checkLoginStreak(ctx context.Context, uid int64) (int64, bool) {
	today := time.Now().Format("2006-01-02")
	lastKey := fmt.Sprintf("pts:streak:%d:last", uid)
	countKey := fmt.Sprintf("pts:streak:%d:count", uid)

	lastDate, err := s.rdb.Get(ctx, lastKey).Result()
	if err != nil {
		// Redis miss — load from MySQL
		var dbDate sql.NullString
		var dbCount int64
		if s.db != nil {
			s.db.QueryRow("SELECT last_login_date, streak_count FROM point_accounts WHERE user_id=?", uid).
				Scan(&dbDate, &dbCount)
			if dbDate.Valid {
				lastDate = dbDate.String
				s.rdb.Set(ctx, lastKey, lastDate, 48*time.Hour)
				s.rdb.Set(ctx, countKey, dbCount, 7*24*time.Hour)
			}
		}
	}

	if lastDate == today {
		streak, _ := s.rdb.Get(ctx, countKey).Int64()
		return streak, false
	}

	yesterday := time.Now().AddDate(0, 0, -1).Format("2006-01-02")
	var streak int64
	if lastDate == yesterday {
		streak = s.rdb.Incr(ctx, countKey).Val()
	} else {
		s.rdb.Set(ctx, countKey, 1, 7*24*time.Hour)
		streak = 1
	}
	s.rdb.Set(ctx, lastKey, today, 48*time.Hour)

	// Fire-and-forget sync to MySQL
	if s.db != nil {
		go s.db.Exec("UPDATE point_accounts SET streak_count=?, last_login_date=? WHERE user_id=?",
			streak, today, uid)
	}

	return streak, streak%7 == 0
}

// ---- Event consumer (RabbitMQ) ----

func (s *pointsServer) consumeEvents() {
	msgs, err := s.amqpCh.Consume("pts.earn", "", false, false, false, false, nil)
	if err != nil {
		slog.Error("pts: consume failed", "error", err)
		return
	}
	slog.Info("points: consuming pts.* from RabbitMQ")
	for msg := range msgs {
		var ev struct {
			Type           string `json:"type"`
			UserID         int64  `json:"user_id"`
			IdempotencyKey string `json:"key"`
			Amount         int64  `json:"amount"`
		}
		if err := json.Unmarshal(msg.Body, &ev); err != nil {
			msg.Nack(false, false)
			continue
		}

		amount := int64(0)
		limit := 0
		switch ev.Type {
		case "sheet.created":
			amount = 5
			limit = 5
		case "file.uploaded":
			amount = 3
			limit = 10
		case "order.created":
			if ev.Amount <= 0 {
				msg.Ack(false)
				continue
			}
			ctxDed, cancelDed := context.WithTimeout(context.Background(), 3*time.Second)
			s.Deduct(ctxDed, &pb.DeductRequest{
				UserId: ev.UserID, Amount: ev.Amount,
				Reason: "seckill_order", RefId: ev.IdempotencyKey,
			})
			cancelDed()
			msg.Ack(false)
			continue
		default:
			msg.Nack(false, false)
			continue
		}

		if limit > 0 {
			limitKey := fmt.Sprintf("pts:limit:%d:%s:%s", ev.UserID, ev.Type, time.Now().Format("2006-01-02"))
			n, _ := s.rdb.Incr(context.Background(), limitKey).Result()
			s.rdb.Expire(context.Background(), limitKey, 24*time.Hour)
			if n > int64(limit) {
				msg.Ack(false)
				continue
			}
		}

		ctxEarn, cancelEarn := context.WithTimeout(context.Background(), 3*time.Second)
		_, _ = s.Earn(ctxEarn, &pb.EarnRequest{
			UserId: ev.UserID, Amount: amount, Reason: ev.Type, IdempotencyKey: ev.IdempotencyKey,
		})
		cancelEarn()
		msg.Ack(false)
	}
}

func (s *pointsServer) balanceFromMySQL(ctx context.Context, uid int64) int64 {
	var b int64
	s.db.QueryRow("SELECT balance FROM point_accounts WHERE user_id=?", uid).Scan(&b)
	return b
}

func main() {
	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	port := getenv("PORT", "50052")
	mysqlDSN := fmt.Sprintf("%s:%s@tcp(%s)/%s?parseTime=true",
		getenv("MYSQL_USER", "root"), getenv("MYSQL_PASSWORD", "123456"),
		getenv("MYSQL_ADDR", "mysql-auth:3306"), getenv("MYSQL_DB", "rpc_auth"))
	amqpAddr := getenv("RABBITMQ_ADDR", "amqp://rpc:rpc-rabbit-123456@rabbitmq:5672/")

	rdb := redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})
	db := initDB(mysqlDSN)
	amqpCh := initAMQP(amqpAddr)

	srvImpl := &pointsServer{db: db, rdb: rdb, amqpCh: amqpCh}
	go srvImpl.consumeEvents()

	lis, _ := net.Listen("tcp", ":"+port)
	srv := grpc.NewServer()
	pb.RegisterPointsServiceServer(srv, srvImpl)
	hs := health.NewServer()
	hs.SetServingStatus("", grpc_health_v1.HealthCheckResponse_SERVING)
	grpc_health_v1.RegisterHealthServer(srv, hs)
	reflection.Register(srv)
	registerConsul(port)
	slog.Info("points-server listening", "port", port)
	srv.Serve(lis)
}

func getenv(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

func registerConsul(port string) {
	consul := os.Getenv("CONSUL_HTTP_ADDR")
	if consul == "" {
		consul = "http://consul:8500"
	}
	hostname, _ := os.Hostname()
	myIP := os.Getenv("HOST_IP")
	if myIP == "" {
		myIP = "127.0.0.1"
	}
	body := fmt.Sprintf(`{"ID":"points-%s","Name":"rpc-points","Address":"%s","Port":%s,"Check":{"Name":"points gRPC","GRPC":"%s:%s","GRPCUseTLS":false,"Interval":"10s","Timeout":"3s","DeregisterCriticalServiceAfter":"90s"}}`, hostname, myIP, port, myIP, port)
	go func() { http.Post(consul+"/v1/agent/service/register", "application/json", strings.NewReader(body)) }()
}
