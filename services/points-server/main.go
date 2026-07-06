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

func main() {
	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	port := getenv("PORT", "50052")

	rdb := redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})

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
