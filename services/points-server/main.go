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
	"strconv"
	"strings"
	"time"

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
	db  *sql.DB
	rdb *redis.Client
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

func migrate(db *sql.DB) {
	ddls := []string{
		`CREATE TABLE IF NOT EXISTS point_accounts (
			user_id BIGINT PRIMARY KEY,
			balance BIGINT NOT NULL DEFAULT 0,
			total_earned BIGINT NOT NULL DEFAULT 0,
			updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
		)`,
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

// ---- GetBalance ----

func (s *pointsServer) GetBalance(ctx context.Context, req *pb.GetBalanceRequest) (*pb.BalanceResponse, error) {
	// Read Redis cache first
	key := fmt.Sprintf("pts:%d", req.UserId)
	bal, err := s.rdb.HGet(ctx, key, "balance").Int64()
	if err == nil {
		total, _ := s.rdb.HGet(ctx, key, "total_earned").Int64()
		return &pb.BalanceResponse{Success: true, UserId: req.UserId, Balance: bal, TotalEarned: total}, nil
	}
	// Fallback to MySQL
	var balance, totalEarned int64
	s.db.QueryRow("SELECT balance, total_earned FROM point_accounts WHERE user_id=?", req.UserId).
		Scan(&balance, &totalEarned)
	// Write back to Redis
	s.rdb.HSet(ctx, key, "balance", balance, "total_earned", totalEarned)
	s.rdb.Expire(ctx, key, 5*time.Minute)
	return &pb.BalanceResponse{Success: true, UserId: req.UserId, Balance: balance, TotalEarned: totalEarned}, nil
}

// ---- GetTransactions ----

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

// ---- Earn ----

func (s *pointsServer) Earn(ctx context.Context, req *pb.EarnRequest) (*pb.BalanceResponse, error) {
	// Idempotency
	if req.IdempotencyKey != "" {
		ok, _ := s.rdb.SetNX(ctx, "pts:dup:"+req.IdempotencyKey, "1", 5*time.Minute).Result()
		if !ok {
			bal, _ := s.balanceFromMySQL(ctx, req.UserId)
			return &pb.BalanceResponse{Success: false, Balance: bal, Error: "duplicate"}, nil
		}
	}

	tx, _ := s.db.Begin()
	defer tx.Rollback()

	// Upsert account
	tx.Exec("INSERT INTO point_accounts (user_id, balance, total_earned) VALUES (?,?,?) ON DUPLICATE KEY UPDATE balance=balance+?, total_earned=total_earned+?", req.UserId, req.Amount, req.Amount, req.Amount, req.Amount)

	// Insert transaction
	res, err := tx.Exec("INSERT INTO point_transactions (user_id, type, amount, reason) VALUES (?,?,?,?)", req.UserId, "earn", req.Amount, req.Reason)
	if err != nil {
		return &pb.BalanceResponse{Success: false, Error: "db error"}, nil
	}
	txID, _ := res.LastInsertId()
	tx.Commit()

	// Update Redis cache
	key := fmt.Sprintf("pts:%d", req.UserId)
	pipe := s.rdb.Pipeline()
	pipe.HIncrBy(ctx, key, "balance", req.Amount)
	pipe.HIncrBy(ctx, key, "total_earned", req.Amount)
	pipe.Expire(ctx, key, 5*time.Minute)
	cmds, _ := pipe.Exec(ctx)

	balance := cmds[0].(*redis.IntCmd).Val()
	total := cmds[1].(*redis.IntCmd).Val()

	// Redis transaction log (hot cache, optional)
	b, _ := json.Marshal(map[string]interface{}{"id": txID, "type": "earn", "amount": req.Amount, "reason": req.Reason, "created_at": time.Now().Format(time.RFC3339)})
	s.rdb.LPush(ctx, fmt.Sprintf("pts:tx:%d", req.UserId), string(b))
	s.rdb.LTrim(ctx, fmt.Sprintf("pts:tx:%d", req.UserId), 0, 99)

	return &pb.BalanceResponse{Success: true, UserId: req.UserId, Balance: balance, TotalEarned: total}, nil
}

// ---- Deduct ----

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
	tx.Exec("INSERT INTO point_transactions (user_id, type, amount, reason, ref_id) VALUES (?,?,?,?,?)", req.UserId, "deduct", -req.Amount, req.Reason, req.RefId)
	tx.Commit()

	newBal := balance - req.Amount

	// Update Redis
	key := fmt.Sprintf("pts:%d", req.UserId)
	s.rdb.HIncrBy(ctx, key, "balance", -req.Amount)
	s.rdb.Expire(ctx, key, 5*time.Minute)

	return &pb.BalanceResponse{Success: true, UserId: req.UserId, Balance: newBal}, nil
}

// ---- GetLeaderboard ----

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

// ---- Streak (Redis only, no DB needed) ----

func (s *pointsServer) checkLoginStreak(ctx context.Context, uid int64) (int64, bool) {
	today := time.Now().Format("2006-01-02")
	lastKey := fmt.Sprintf("pts:streak:%d:last", uid)
	countKey := fmt.Sprintf("pts:streak:%d:count", uid)
	lastDate, _ := s.rdb.Get(ctx, lastKey).Result()
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
	return streak, streak%7 == 0
}

// ---- Event consumer ----

func (s *pointsServer) consumeEvents() {
	pubsub := s.rdb.Subscribe(context.Background(), "pts:earn")
	defer pubsub.Close()
	ch := pubsub.Channel()

	slog.Info("points: consuming events from Redis pts:earn")
	for msg := range ch {
		var ev struct {
			Type           string `json:"type"`
			UserID         int64  `json:"user_id"`
			IdempotencyKey string `json:"key"`
			Amount         int64  `json:"amount"`
		}
		if err := json.Unmarshal([]byte(msg.Payload), &ev); err != nil {
			continue
		}

		amount := int64(0)
		limit := 0
		switch ev.Type {
		case "user.logged_in":
			amount = 10
			limit = 1
			if streak, bonus := s.checkLoginStreak(context.Background(), ev.UserID); bonus {
				ctx2, cancel2 := context.WithTimeout(context.Background(), 3*time.Second)
				s.Earn(ctx2, &pb.EarnRequest{
					UserId: ev.UserID, Amount: 50, Reason: "login_streak_7day",
					IdempotencyKey: fmt.Sprintf("streak:%d:%d", ev.UserID, streak),
				})
				cancel2()
			}
		case "sheet.created":
			amount = 5
			limit = 5
		case "file.uploaded":
			amount = 3
			limit = 10
		case "seckill_order":
			ctx2, cancel2 := context.WithTimeout(context.Background(), 3*time.Second)
			defer cancel2()
			s.Deduct(ctx2, &pb.DeductRequest{
				UserId: ev.UserID, Amount: ev.Amount,
				Reason: "seckill_order", RefId: ev.IdempotencyKey,
			})
			continue
		default:
			continue
		}

		if limit > 0 {
			limitKey := fmt.Sprintf("pts:limit:%d:%s:%s", ev.UserID, ev.Type, time.Now().Format("2006-01-02"))
			n, _ := s.rdb.Incr(context.Background(), limitKey).Result()
			s.rdb.Expire(context.Background(), limitKey, 24*time.Hour)
			if n > int64(limit) {
				continue
			}
		}

		ctx2, cancel2 := context.WithTimeout(context.Background(), 3*time.Second)
		_, _ = s.Earn(ctx2, &pb.EarnRequest{
			UserId: ev.UserID, Amount: amount, Reason: ev.Type, IdempotencyKey: ev.IdempotencyKey,
		})
		cancel2()
	}
}

func (s *pointsServer) balanceFromMySQL(ctx context.Context, uid int64) int64 {
	var b int64
	s.db.QueryRow("SELECT balance FROM point_accounts WHERE user_id=?", uid).Scan(&b)
	return b
}

// ---- main ----

func main() {
	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	port := getenv("PORT", "50052")
	mysqlDSN := fmt.Sprintf("%s:%s@tcp(%s)/%s?parseTime=true",
		getenv("MYSQL_USER", "root"), getenv("MYSQL_PASSWORD", "123456"),
		getenv("MYSQL_ADDR", "mysql-auth:3306"), getenv("MYSQL_DB", "rpc_auth"))

	rdb := redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})
	db := initDB(mysqlDSN)

	srvImpl := &pointsServer{db: db, rdb: rdb}
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
