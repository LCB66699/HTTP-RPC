package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"

	pb "gateway-grpc/gen/rpc"
	"github.com/redis/go-redis/v9"
	"google.golang.org/grpc"
	"google.golang.org/grpc/health"
	"google.golang.org/grpc/health/grpc_health_v1"
	"google.golang.org/grpc/reflection"
)

type pointsServer struct {
	pb.UnimplementedPointsServiceServer
	rdb *redis.Client
}

func (s *pointsServer) GetBalance(ctx context.Context, req *pb.GetBalanceRequest) (*pb.BalanceResponse, error) {
	key := fmt.Sprintf("pts:%d", req.UserId)
	vals, err := s.rdb.HMGet(ctx, key, "balance", "total_earned").Result()
	if err != nil {
		return &pb.BalanceResponse{Success: false, Error: "redis error"}, nil
	}

	var balance, total int64
	if v, ok := vals[0].(string); ok {
		balance, _ = strconv.ParseInt(v, 10, 64)
	}
	if v, ok := vals[1].(string); ok {
		total, _ = strconv.ParseInt(v, 10, 64)
	}
	return &pb.BalanceResponse{
		Success: true, UserId: req.UserId,
		Balance: balance, TotalEarned: total,
	}, nil
}

func (s *pointsServer) GetTransactions(ctx context.Context, req *pb.GetTransactionsRequest) (*pb.TransactionsResponse, error) {
	limit := req.Limit
	if limit <= 0 || limit > 100 {
		limit = 20
	}
	vals, err := s.rdb.LRange(ctx, fmt.Sprintf("pts:tx:%d", req.UserId), 0, int64(limit-1)).Result()
	if err != nil {
		return &pb.TransactionsResponse{Success: false, Error: "redis error"}, nil
	}

	resp := &pb.TransactionsResponse{Success: true}
	for _, v := range vals {
		var tx pb.Transaction
		if err := json.Unmarshal([]byte(v), &tx); err == nil {
			resp.Transactions = append(resp.Transactions, &tx)
		}
	}
	return resp, nil
}

func (s *pointsServer) Earn(ctx context.Context, req *pb.EarnRequest) (*pb.BalanceResponse, error) {
	if req.IdempotencyKey != "" {
		dupKey := fmt.Sprintf("pts:dup:%s", req.IdempotencyKey)
		ok, _ := s.rdb.SetNX(ctx, dupKey, "1", 5*time.Minute).Result()
		if !ok {
			bal := s.balance(ctx, req.UserId)
			return &pb.BalanceResponse{
				Success: false, UserId: req.UserId, Balance: bal,
				Error: "duplicate earn request",
			}, nil
		}
	}

	key := fmt.Sprintf("pts:%d", req.UserId)
	pipe := s.rdb.Pipeline()
	pipe.HIncrBy(ctx, key, "balance", req.Amount)
	pipe.HIncrBy(ctx, key, "total_earned", req.Amount)
	cmds, err := pipe.Exec(ctx)
	if err != nil {
		return &pb.BalanceResponse{Success: false, Error: "redis error"}, nil
	}
	balance := cmds[0].(*redis.IntCmd).Val()

	tx := pb.Transaction{
		Type:      "earn",
		Amount:    req.Amount,
		Reason:    req.Reason,
		CreatedAt: time.Now().Format(time.RFC3339),
	}
	b, _ := json.Marshal(&tx)
	s.rdb.LPush(ctx, fmt.Sprintf("pts:tx:%d", req.UserId), string(b))
	s.rdb.LTrim(ctx, fmt.Sprintf("pts:tx:%d", req.UserId), 0, 99)

	return &pb.BalanceResponse{
		Success: true, UserId: req.UserId,
		Balance:     balance,
		TotalEarned: cmds[1].(*redis.IntCmd).Val(),
	}, nil
}

func (s *pointsServer) Deduct(ctx context.Context, req *pb.DeductRequest) (*pb.BalanceResponse, error) {
	key := fmt.Sprintf("pts:%d", req.UserId)
	newBal, err := s.rdb.HIncrBy(ctx, key, "balance", -req.Amount).Result()
	if err != nil {
		return &pb.BalanceResponse{Success: false, Error: "redis error"}, nil
	}
	if newBal < 0 {
		s.rdb.HIncrBy(ctx, key, "balance", req.Amount)
		bal := s.balance(ctx, req.UserId)
		return &pb.BalanceResponse{
			Success: false, UserId: req.UserId, Balance: bal,
			Error: "insufficient balance",
		}, nil
	}

	tx := pb.Transaction{
		Type:      "deduct",
		Amount:    -req.Amount,
		Reason:    req.Reason,
		CreatedAt: time.Now().Format(time.RFC3339),
	}
	b, _ := json.Marshal(&tx)
	s.rdb.LPush(ctx, fmt.Sprintf("pts:tx:%d", req.UserId), string(b))

	return &pb.BalanceResponse{
		Success: true, UserId: req.UserId,
		Balance: newBal,
	}, nil
}

func (s *pointsServer) balance(ctx context.Context, uid int64) int64 {
	v, _ := s.rdb.HGet(ctx, fmt.Sprintf("pts:%d", uid), "balance").Int64()
	return v
}

// checkLoginStreak returns current streak count and whether a 7-day bonus is triggered.
func (s *pointsServer) checkLoginStreak(ctx context.Context, uid int64) (int, bool) {
	today := time.Now().Format("2006-01-02")
	lastKey := fmt.Sprintf("pts:streak:%d:last", uid)
	countKey := fmt.Sprintf("pts:streak:%d:count", uid)

	lastDate, _ := s.rdb.Get(ctx, lastKey).Result()
	if lastDate == today {
		streak, _ := s.rdb.Get(ctx, countKey).Int()
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
	return int(streak), streak%7 == 0
}

// consumeEvents listens on Redis "pts:earn" channel for point-earning events.
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
			// Check streak bonus
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
		default:
			continue
		}

		// Daily limit check
		if limit > 0 {
			limitKey := fmt.Sprintf("pts:limit:%d:%s:%s", ev.UserID, ev.Type, time.Now().Format("2006-01-02"))
			n, _ := s.rdb.Incr(context.Background(), limitKey).Result()
			s.rdb.Expire(context.Background(), limitKey, 24*time.Hour)
			if n > int64(limit) {
				continue
			}
		}

		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		_, err := s.Earn(ctx, &pb.EarnRequest{
			UserId:         ev.UserID,
			Amount:         amount,
			Reason:         ev.Type,
			IdempotencyKey: ev.IdempotencyKey,
		})
		cancel()
		if err != nil {
			slog.Warn("points: earn failed", "type", ev.Type, "uid", ev.UserID)
		}
	}
}

func main() {
	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	port := getenv("PORT", "50052")

	rdb := redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})

	srvImpl := &pointsServer{rdb: rdb}
	go srvImpl.consumeEvents()

	lis, err := net.Listen("tcp", ":"+port)
	if err != nil {
		slog.Error("failed to listen", "error", err)
		os.Exit(1)
	}

	srv := grpc.NewServer()
	pb.RegisterPointsServiceServer(srv, &pointsServer{rdb: rdb})

	hs := health.NewServer()
	hs.SetServingStatus("", grpc_health_v1.HealthCheckResponse_SERVING)
	grpc_health_v1.RegisterHealthServer(srv, hs)
	reflection.Register(srv)

	// Consul registration
	registerConsul(port)

	slog.Info("points-server listening", "port", port)
	if err := srv.Serve(lis); err != nil {
		slog.Error("failed to serve", "error", err)
		os.Exit(1)
	}
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

	body := fmt.Sprintf(`{
		"ID": "points-%s",
		"Name": "rpc-points",
		"Address": "%s",
		"Port": %s,
		"Check": {
			"Name": "points gRPC health",
			"GRPC": "%s:%s",
			"GRPCUseTLS": false,
			"Interval": "10s",
			"Timeout": "3s",
			"DeregisterCriticalServiceAfter": "90s"
		}
	}`, hostname, myIP, port, myIP, port)

	go func() {
		http.Post(consul+"/v1/agent/service/register", "application/json",
			strings.NewReader(body))
	}()
}
