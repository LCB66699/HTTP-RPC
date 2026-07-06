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

type mallServer struct {
	pb.UnimplementedMallServiceServer
	rdb *redis.Client
}

func (s *mallServer) initSampleData() {
	ctx := context.Background()
	products := []map[string]interface{}{
		{"name": "10 积分礼包", "price": int64(10), "stock": int64(999), "desc": "小额积分兑换"},
		{"name": "50 积分礼包", "price": int64(50), "stock": int64(500), "desc": "中等积分兑换"},
		{"name": "100 积分礼包", "price": int64(100), "stock": int64(100), "desc": "大额积分兑换"},
	}
	for i, p := range products {
		b, _ := json.Marshal(p)
		s.rdb.Set(ctx, fmt.Sprintf("mall:product:%d", i+1), string(b), 0)
	}
}

func (s *mallServer) getProduct(ctx context.Context, id int64) (*pb.Product, error) {
	data, err := s.rdb.Get(ctx, fmt.Sprintf("mall:product:%d", id)).Result()
	if err != nil {
		return nil, err
	}
	var p struct {
		Name  string `json:"name"`
		Price int64  `json:"price"`
		Stock int64  `json:"stock"`
		Desc  string `json:"desc"`
	}
	json.Unmarshal([]byte(data), &p)
	return &pb.Product{
		Id: id, Name: p.Name, Price: p.Price, Stock: p.Stock,
		Description: p.Desc,
	}, nil
}

// ---- Products ----

func (s *mallServer) ListProducts(ctx context.Context, req *pb.ListProductsRequest) (*pb.ListProductsResponse, error) {
	resp := &pb.ListProductsResponse{Success: true}
	for id := int64(1); ; id++ {
		p, err := s.getProduct(ctx, id)
		if err != nil {
			break
		}
		resp.Products = append(resp.Products, p)
	}
	return resp, nil
}

func (s *mallServer) GetProduct(ctx context.Context, req *pb.GetProductRequest) (*pb.ProductResponse, error) {
	p, err := s.getProduct(ctx, req.Id)
	if err != nil {
		return &pb.ProductResponse{Success: false, Error: "Not found", ErrorCode: 4}, nil
	}
	return &pb.ProductResponse{Success: true, Product: p}, nil
}

func (s *mallServer) CreateProduct(ctx context.Context, req *pb.CreateProductRequest) (*pb.ProductResponse, error) {
	id := s.rdb.Incr(ctx, "mall:product:seq").Val()
	data := map[string]interface{}{
		"name":  req.Name,
		"price": req.Price,
		"stock": req.Stock,
		"desc":  req.Description,
	}
	b, _ := json.Marshal(data)
	s.rdb.Set(ctx, fmt.Sprintf("mall:product:%d", id), string(b), 0)
	return &pb.ProductResponse{
		Success: true,
		Product: &pb.Product{Id: id, Name: req.Name, Price: req.Price, Stock: req.Stock, Description: req.Description},
	}, nil
}

// ---- Seckill ----

func (s *mallServer) ListSeckills(ctx context.Context, req *pb.ListSeckillsRequest) (*pb.ListSeckillsResponse, error) {
	resp := &pb.ListSeckillsResponse{Success: true}
	for id := int64(1); ; id++ {
		data, err := s.rdb.Get(ctx, fmt.Sprintf("mall:seckill:%d", id)).Result()
		if err != nil {
			break
		}
		var sk struct {
			ProductID     int64  `json:"product_id"`
			SeckillPrice  int64  `json:"seckill_price"`
			SeckillStock  int64  `json:"seckill_stock"`
			StartAt       int64  `json:"start_at"`
			EndAt         int64  `json:"end_at"`
			ProductName   string `json:"product_name"`
		}
		json.Unmarshal([]byte(data), &sk)
		resp.Seckills = append(resp.Seckills, &pb.Seckill{
			Id: id, ProductId: sk.ProductID, SeckillPrice: sk.SeckillPrice,
			SeckillStock: sk.SeckillStock, StartAt: sk.StartAt, EndAt: sk.EndAt,
			ProductName: sk.ProductName,
		})
	}
	return resp, nil
}

func (s *mallServer) CreateSeckill(ctx context.Context, req *pb.CreateSeckillRequest) (*pb.SeckillResponse, error) {
	id := s.rdb.Incr(ctx, "mall:seckill:seq").Val()

	// Get product name
	p, _ := s.getProduct(ctx, req.ProductId)
	pName := ""
	if p != nil {
		pName = p.Name
	}

	sk := map[string]interface{}{
		"product_id":    req.ProductId,
		"seckill_price": req.SeckillPrice,
		"seckill_stock": req.SeckillStock,
		"start_at":      req.StartAt,
		"end_at":        req.EndAt,
		"product_name":  pName,
	}
	b, _ := json.Marshal(sk)
	s.rdb.Set(ctx, fmt.Sprintf("mall:seckill:%d", id), string(b), 0)

	// Init stock in Redis
	s.rdb.Set(ctx, fmt.Sprintf("seckill:stock:%d", id), req.SeckillStock, 0)

	return &pb.SeckillResponse{
		Success: true,
		Seckill: &pb.Seckill{
			Id: id, ProductId: req.ProductId, SeckillPrice: req.SeckillPrice,
			SeckillStock: req.SeckillStock, StartAt: req.StartAt, EndAt: req.EndAt,
			ProductName: pName,
		},
	}, nil
}

// ---- Orders ----

func (s *mallServer) SeckillOrder(ctx context.Context, req *pb.SeckillOrderRequest) (*pb.OrderResponse, error) {
	// Idempotency check
	if req.IdempotencyKey != "" {
		ok, _ := s.rdb.SetNX(ctx, "mall:dup:"+req.IdempotencyKey, "1", 60*time.Second).Result()
		if !ok {
			return &pb.OrderResponse{Success: false, Error: "duplicate order"}, nil
		}
	}

	// Load seckill
	data, err := s.rdb.Get(ctx, fmt.Sprintf("mall:seckill:%d", req.SeckillId)).Result()
	if err != nil {
		return &pb.OrderResponse{Success: false, Error: "seckill not found"}, nil
	}
	var sk struct {
		ProductID    int64 `json:"product_id"`
		SeckillPrice int64 `json:"seckill_price"`
		StartAt      int64 `json:"start_at"`
		EndAt        int64 `json:"end_at"`
		ProductName  string `json:"product_name"`
	}
	json.Unmarshal([]byte(data), &sk)

	// Time window check
	now := time.Now().Unix()
	if now < sk.StartAt {
		return &pb.OrderResponse{Success: false, Error: "seckill not started"}, nil
	}
	if now > sk.EndAt {
		return &pb.OrderResponse{Success: false, Error: "seckill ended"}, nil
	}

	// Stock check (atomic decrement)
	stockKey := fmt.Sprintf("seckill:stock:%d", req.SeckillId)
	newStock, err := s.rdb.Decr(ctx, stockKey).Result()
	if err != nil {
		return &pb.OrderResponse{Success: false, Error: "stock error"}, nil
	}
	if newStock < 0 {
		s.rdb.Incr(ctx, stockKey) // rollback
		return &pb.OrderResponse{Success: false, Error: "sold out"}, nil
	}

	// Create order
	orderID := s.rdb.Incr(ctx, "mall:order:seq").Val()
	order := map[string]interface{}{
		"user_id":     req.UserId,
		"product_id":  sk.ProductID,
		"seckill_id":  req.SeckillId,
		"amount":      sk.SeckillPrice,
		"status":      "paid",
		"created_at":  time.Now().Format(time.RFC3339),
		"product_name": sk.ProductName,
	}
	b, _ := json.Marshal(order)
	s.rdb.Set(ctx, fmt.Sprintf("mall:order:%d", orderID), string(b), 0)
	s.rdb.LPush(ctx, fmt.Sprintf("mall:orders:%d", req.UserId), strconv.FormatInt(orderID, 10))

	// Deduct points via points service event
	s.rdb.Publish(ctx, "pts:earn", toJSON(map[string]interface{}{
		"type":    "seckill_order",
		"user_id": req.UserId,
		"amount":  sk.SeckillPrice,
		"key":     fmt.Sprintf("seckill:%d:%d", req.SeckillId, req.UserId),
	}))

	return &pb.OrderResponse{
		Success: true,
		Order: &pb.Order{
			Id: orderID, UserId: req.UserId, ProductId: sk.ProductID,
			SeckillId: req.SeckillId, Amount: sk.SeckillPrice,
			Status: "paid", CreatedAt: time.Now().Format(time.RFC3339),
			ProductName: sk.ProductName,
		},
	}, nil
}

func (s *mallServer) ListOrders(ctx context.Context, req *pb.ListOrdersRequest) (*pb.ListOrdersResponse, error) {
	resp := &pb.ListOrdersResponse{Success: true}
	orderIDs, _ := s.rdb.LRange(ctx, fmt.Sprintf("mall:orders:%d", req.UserId), 0, 49).Result()
	for _, idStr := range orderIDs {
		data, err := s.rdb.Get(ctx, fmt.Sprintf("mall:order:%s", idStr)).Result()
		if err != nil {
			continue
		}
		var o struct {
			UserID      int64  `json:"user_id"`
			ProductID   int64  `json:"product_id"`
			SeckillID   int64  `json:"seckill_id"`
			Amount      int64  `json:"amount"`
			Status      string `json:"status"`
			CreatedAt   string `json:"created_at"`
			ProductName string `json:"product_name"`
		}
		json.Unmarshal([]byte(data), &o)
		id, _ := strconv.ParseInt(idStr, 10, 64)
		resp.Orders = append(resp.Orders, &pb.Order{
			Id: id, UserId: o.UserID, ProductId: o.ProductID,
			SeckillId: o.SeckillID, Amount: o.Amount,
			Status: o.Status, CreatedAt: o.CreatedAt, ProductName: o.ProductName,
		})
	}
	return resp, nil
}

func toJSON(v interface{}) string {
	b, _ := json.Marshal(v)
	return string(b)
}

func main() {
	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	port := getenv("PORT", "50053")

	rdb := redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})

	srvImpl := &mallServer{rdb: rdb}
	srvImpl.initSampleData()

	lis, _ := net.Listen("tcp", ":"+port)
	srv := grpc.NewServer()
	pb.RegisterMallServiceServer(srv, srvImpl)

	hs := health.NewServer()
	hs.SetServingStatus("", grpc_health_v1.HealthCheckResponse_SERVING)
	grpc_health_v1.RegisterHealthServer(srv, hs)
	reflection.Register(srv)

	// Consul registration
	registerConsul(port)

	slog.Info("mall-server listening", "port", port)
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
	body := fmt.Sprintf(`{
		"ID": "mall-%s","Name": "rpc-mall","Address": "%s","Port": %s,
		"Check": {"Name": "mall gRPC","GRPC": "%s:%s","GRPCUseTLS": false,
		"Interval": "10s","Timeout": "3s","DeregisterCriticalServiceAfter": "90s"}
	}`, hostname, myIP, port, myIP, port)
	go func() {
		http.Post(consul+"/v1/agent/service/register", "application/json", strings.NewReader(body))
	}()
}
