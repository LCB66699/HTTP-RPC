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

	amqp "github.com/rabbitmq/amqp091-go"
	_ "github.com/go-sql-driver/mysql"
	pb "gateway-grpc/gen/rpc"
	"github.com/redis/go-redis/v9"
	"google.golang.org/grpc"
	"google.golang.org/grpc/health"
	"google.golang.org/grpc/health/grpc_health_v1"
	"google.golang.org/grpc/reflection"
)

type mallServer struct {
	pb.UnimplementedMallServiceServer
	db      *sql.DB
	rdb     *redis.Client
	amqpCh  *amqp.Channel
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
	q, _ := ch.QueueDeclare("mall.order", true, false, false, false, nil)
	ch.QueueBind(q.Name, "order.created", "rpc.events", false, nil)
	return ch
}

func initDB(dsn string) *sql.DB {
	db, err := sql.Open("mysql", dsn)
	if err != nil {
		slog.Error("mysql open failed", "error", err)
		os.Exit(1)
	}
	db.SetMaxOpenConns(15)
	db.SetMaxIdleConns(5)
	db.SetConnMaxLifetime(5 * time.Minute)
	if err := db.Ping(); err != nil {
		slog.Error("mysql ping failed", "error", err)
		os.Exit(1)
	}
	migrate(db)
	seed(db)
	return db
}

func migrate(db *sql.DB) {
	ddls := []string{
		`CREATE TABLE IF NOT EXISTS products (
			id BIGINT PRIMARY KEY,
			name VARCHAR(128) NOT NULL,
			description TEXT,
			price BIGINT NOT NULL,
			stock BIGINT NOT NULL DEFAULT 0,
			image_url VARCHAR(512) DEFAULT '',
			created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)`,
		`CREATE TABLE IF NOT EXISTS seckills (
			id BIGINT PRIMARY KEY,
			product_id BIGINT NOT NULL,
			seckill_price BIGINT NOT NULL,
			seckill_stock BIGINT NOT NULL,
			start_at BIGINT NOT NULL,
			end_at BIGINT NOT NULL,
			created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
		)`,
		`CREATE TABLE IF NOT EXISTS orders (
			id BIGINT PRIMARY KEY,
			user_id BIGINT NOT NULL,
			product_id BIGINT NOT NULL,
			seckill_id BIGINT DEFAULT 0,
			amount BIGINT NOT NULL,
			status VARCHAR(16) NOT NULL DEFAULT 'paid',
			idempotency_key VARCHAR(128) DEFAULT '',
			created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
			INDEX idx_user (user_id),
			UNIQUE KEY uk_idempotency (idempotency_key)
		)`,
	}
	for _, ddl := range ddls {
		if _, err := db.Exec(ddl); err != nil {
			slog.Warn("migrate warning", "error", err)
		}
	}
}

func seed(db *sql.DB) {
	var cnt int
	db.QueryRow("SELECT COUNT(*) FROM products").Scan(&cnt)
	if cnt > 0 {
		return
	}
	items := []struct {
		id    int64
		name  string
		price int64
		stock int64
		desc  string
	}{
		{1, "10 积分礼包", 10, 999, "小额积分兑换"},
		{2, "50 积分礼包", 50, 500, "中等积分兑换"},
		{3, "100 积分礼包", 100, 100, "大额积分兑换"},
	}
	for _, item := range items {
		db.Exec("INSERT INTO products (id,name,description,price,stock) VALUES (?,?,?,?,?)",
			item.id, item.name, item.desc, item.price, item.stock)
	}
}

// ---- Products ----

func (s *mallServer) ListProducts(ctx context.Context, req *pb.ListProductsRequest) (*pb.ListProductsResponse, error) {
	rows, err := s.db.Query("SELECT id, name, description, price, stock, image_url FROM products ORDER BY id")
	if err != nil {
		return &pb.ListProductsResponse{Success: false, Error: err.Error()}, nil
	}
	defer rows.Close()

	resp := &pb.ListProductsResponse{Success: true}
	for rows.Next() {
		p := &pb.Product{}
		rows.Scan(&p.Id, &p.Name, &p.Description, &p.Price, &p.Stock, &p.ImageUrl)
		resp.Products = append(resp.Products, p)
	}
	return resp, nil
}

func (s *mallServer) GetProduct(ctx context.Context, req *pb.GetProductRequest) (*pb.ProductResponse, error) {
	p := &pb.Product{}
	err := s.db.QueryRow("SELECT id, name, description, price, stock, image_url FROM products WHERE id=?", req.Id).
		Scan(&p.Id, &p.Name, &p.Description, &p.Price, &p.Stock, &p.ImageUrl)
	if err != nil {
		return &pb.ProductResponse{Success: false, Error: "Not found"}, nil
	}
	return &pb.ProductResponse{Success: true, Product: p}, nil
}

func (s *mallServer) CreateProduct(ctx context.Context, req *pb.CreateProductRequest) (*pb.ProductResponse, error) {
	id := s.rdb.Incr(ctx, "mall:seq:product").Val()
	_, err := s.db.Exec("INSERT INTO products (id,name,description,price,stock,image_url) VALUES (?,?,?,?,?,?)",
		id, req.Name, req.Description, req.Price, req.Stock, req.ImageUrl)
	if err != nil {
		return &pb.ProductResponse{Success: false, Error: err.Error()}, nil
	}
	return &pb.ProductResponse{
		Success: true,
		Product: &pb.Product{Id: id, Name: req.Name, Description: req.Description, Price: req.Price, Stock: req.Stock, ImageUrl: req.ImageUrl},
	}, nil
}

// ---- Seckill ----

func (s *mallServer) ListSeckills(ctx context.Context, req *pb.ListSeckillsRequest) (*pb.ListSeckillsResponse, error) {
	rows, err := s.db.Query("SELECT s.id, s.product_id, p.name, s.seckill_price, s.seckill_stock, s.start_at, s.end_at FROM seckills s LEFT JOIN products p ON s.product_id=p.id ORDER BY s.id DESC")
	if err != nil {
		return &pb.ListSeckillsResponse{Success: false, Error: err.Error()}, nil
	}
	defer rows.Close()

	resp := &pb.ListSeckillsResponse{Success: true}
	for rows.Next() {
		sk := &pb.Seckill{}
		rows.Scan(&sk.Id, &sk.ProductId, &sk.ProductName, &sk.SeckillPrice, &sk.SeckillStock, &sk.StartAt, &sk.EndAt)
		// Override stock with Redis real-time value
		if stock, err := s.rdb.Get(ctx, fmt.Sprintf("seckill:stock:%d", sk.Id)).Int64(); err == nil {
			sk.SeckillStock = stock
		}
		resp.Seckills = append(resp.Seckills, sk)
	}
	return resp, nil
}

func (s *mallServer) CreateSeckill(ctx context.Context, req *pb.CreateSeckillRequest) (*pb.SeckillResponse, error) {
	id := s.rdb.Incr(ctx, "mall:seq:seckill").Val()
	_, err := s.db.Exec("INSERT INTO seckills (id,product_id,seckill_price,seckill_stock,start_at,end_at) VALUES (?,?,?,?,?,?)",
		id, req.ProductId, req.SeckillPrice, req.SeckillStock, req.StartAt, req.EndAt)
	if err != nil {
		return &pb.SeckillResponse{Success: false, Error: err.Error()}, nil
	}

	// Get product name
	var pName string
	s.db.QueryRow("SELECT name FROM products WHERE id=?", req.ProductId).Scan(&pName)

	// Init Redis stock
	s.rdb.Set(ctx, fmt.Sprintf("seckill:stock:%d", id), req.SeckillStock, 0)

	return &pb.SeckillResponse{
		Success: true,
		Seckill: &pb.Seckill{Id: id, ProductId: req.ProductId, ProductName: pName,
			SeckillPrice: req.SeckillPrice, SeckillStock: req.SeckillStock,
			StartAt: req.StartAt, EndAt: req.EndAt},
	}, nil
}

// ---- Orders ----

func (s *mallServer) NormalOrder(ctx context.Context, req *pb.NormalOrderRequest) (*pb.OrderResponse, error) {
	if req.IdempotencyKey != "" {
		ok, _ := s.rdb.SetNX(ctx, "mall:dup:"+req.IdempotencyKey, "1", 60*time.Second).Result()
		if !ok {
			return &pb.OrderResponse{Success: false, Error: "duplicate order"}, nil
		}
	}
	tx, _ := s.db.Begin()
	defer tx.Rollback()
	var stock, price int64
	var pName string
	err := tx.QueryRow("SELECT stock, price, name FROM products WHERE id=? FOR UPDATE", req.ProductId).
		Scan(&stock, &price, &pName)
	if err != nil {
		return &pb.OrderResponse{Success: false, Error: "product not found"}, nil
	}
	if stock <= 0 {
		return &pb.OrderResponse{Success: false, Error: "sold out"}, nil
	}
	tx.Exec("UPDATE products SET stock = stock - 1 WHERE id=?", req.ProductId)
	tx.Commit()

	orderID := s.rdb.Incr(ctx, "mall:seq:order").Val()
	orderMsg, _ := json.Marshal(map[string]interface{}{
		"id": orderID, "user_id": req.UserId, "product_id": req.ProductId,
		"seckill_id": 0, "amount": price,
		"idempotency_key": req.IdempotencyKey, "product_name": pName,
	})
	s.amqpCh.Publish("rpc.events", "order.created", false, false,
		amqp.Publishing{ContentType: "application/json", Body: orderMsg, DeliveryMode: amqp.Persistent})

	return &pb.OrderResponse{
		Success: true,
		Order: &pb.Order{Id: orderID, UserId: req.UserId, ProductId: req.ProductId,
			Amount: price, Status: "paid", ProductName: pName, CreatedAt: time.Now().Format(time.RFC3339)},
	}, nil
}

func (s *mallServer) SeckillOrder(ctx context.Context, req *pb.SeckillOrderRequest) (*pb.OrderResponse, error) {
	// Idempotency
	if req.IdempotencyKey != "" {
		ok, _ := s.rdb.SetNX(ctx, "mall:dup:"+req.IdempotencyKey, "1", 60*time.Second).Result()
		if !ok {
			return &pb.OrderResponse{Success: false, Error: "duplicate order"}, nil
		}
	}

	// Load seckill from MySQL
	var skPID, skPrice, skStart, skEnd int64
	var skName string
	err := s.db.QueryRow("SELECT product_id, seckill_price, start_at, end_at FROM seckills WHERE id=?", req.SeckillId).
		Scan(&skPID, &skPrice, &skStart, &skEnd)
	if err != nil {
		return &pb.OrderResponse{Success: false, Error: "seckill not found"}, nil
	}
	s.db.QueryRow("SELECT name FROM products WHERE id=?", skPID).Scan(&skName)

	// Time window
	now := time.Now().Unix()
	if now < skStart {
		return &pb.OrderResponse{Success: false, Error: "not started"}, nil
	}
	if now > skEnd {
		return &pb.OrderResponse{Success: false, Error: "ended"}, nil
	}

	// Stock check (Redis DECR)
	stockKey := fmt.Sprintf("seckill:stock:%d", req.SeckillId)
	newStock, err := s.rdb.Decr(ctx, stockKey).Result()
	if err != nil {
		return &pb.OrderResponse{Success: false, Error: "stock error"}, nil
	}
	if newStock < 0 {
		s.rdb.Incr(ctx, stockKey)
		return &pb.OrderResponse{Success: false, Error: "sold out"}, nil
	}

	orderID := s.rdb.Incr(ctx, "mall:seq:order").Val()

	// Publish to RabbitMQ (durable, will be consumed to write MySQL)
	orderMsg, _ := json.Marshal(map[string]interface{}{
		"id": orderID, "user_id": req.UserId, "product_id": skPID,
		"seckill_id": req.SeckillId, "amount": skPrice,
		"idempotency_key": req.IdempotencyKey, "product_name": skName,
	})
	err = s.amqpCh.Publish("rpc.events", "order.created", false, false,
		amqp.Publishing{ContentType: "application/json", Body: orderMsg, DeliveryMode: amqp.Persistent})
	if err != nil {
		s.rdb.Incr(ctx, stockKey) // rollback Redis stock
		return &pb.OrderResponse{Success: false, Error: "system busy, retry"}, nil
	}

	return &pb.OrderResponse{
		Success: true,
		Order: &pb.Order{Id: orderID, UserId: req.UserId, ProductId: skPID,
			SeckillId: req.SeckillId, Amount: skPrice,
			Status: "paid", ProductName: skName, CreatedAt: time.Now().Format(time.RFC3339)},
	}, nil
}

func (s *mallServer) ListOrders(ctx context.Context, req *pb.ListOrdersRequest) (*pb.ListOrdersResponse, error) {
	rows, err := s.db.Query("SELECT o.id, o.product_id, o.seckill_id, o.amount, o.status, o.created_at, COALESCE(p.name,'') FROM orders o LEFT JOIN products p ON o.product_id=p.id WHERE o.user_id=? ORDER BY o.created_at DESC LIMIT 50", req.UserId)
	if err != nil {
		return &pb.ListOrdersResponse{Success: true}, nil
	}
	defer rows.Close()

	resp := &pb.ListOrdersResponse{Success: true}
	for rows.Next() {
		o := &pb.Order{UserId: req.UserId}
		rows.Scan(&o.Id, &o.ProductId, &o.SeckillId, &o.Amount, &o.Status, &o.CreatedAt, &o.ProductName)
		resp.Orders = append(resp.Orders, o)
	}
	return resp, nil
}

func (s *mallServer) consumeOrders() {
	msgs, err := s.amqpCh.Consume("mall.order", "", false, false, false, false, nil)
	if err != nil {
		slog.Error("consume orders failed", "error", err)
		return
	}
	slog.Info("mall: consuming order.created from RabbitMQ")
	for msg := range msgs {
		var o struct {
			ID             int64  `json:"id"`
			UserID         int64  `json:"user_id"`
			ProductID      int64  `json:"product_id"`
			SeckillID      int64  `json:"seckill_id"`
			Amount         int64  `json:"amount"`
			IdempotencyKey string `json:"idempotency_key"`
		}
		if err := json.Unmarshal(msg.Body, &o); err != nil {
			msg.Nack(false, false)
			continue
		}
		_, dbErr := s.db.Exec("INSERT INTO orders (id,user_id,product_id,seckill_id,amount,idempotency_key) VALUES (?,?,?,?,?,?)",
			o.ID, o.UserID, o.ProductID, o.SeckillID, o.Amount, o.IdempotencyKey)
		if dbErr != nil {
			slog.Warn("order insert failed, retrying", "id", o.ID, "error", dbErr)
			msg.Nack(false, true) // re-queue
			continue
		}
		msg.Ack(false)
	}
}

// ---- main ----

func main() {
	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	port := getenv("PORT", "50053")
	mysqlDSN := fmt.Sprintf("%s:%s@tcp(%s)/%s?parseTime=true",
		getenv("MYSQL_USER", "root"), getenv("MYSQL_PASSWORD", "123456"),
		getenv("MYSQL_ADDR", "mysql-auth:3306"), getenv("MYSQL_DB", "rpc_auth"))

	rdb := redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})
	db := initDB(mysqlDSN)
	amqpCh := initAMQP(getenv("RABBITMQ_ADDR", "amqp://rpc:rpc-rabbit-123456@rabbitmq:5672/"))

	srvImpl := &mallServer{db: db, rdb: rdb, amqpCh: amqpCh}
	go srvImpl.consumeOrders()

	lis, _ := net.Listen("tcp", ":"+port)
	srv := grpc.NewServer()
	pb.RegisterMallServiceServer(srv, srvImpl)
	hs := health.NewServer()
	hs.SetServingStatus("", grpc_health_v1.HealthCheckResponse_SERVING)
	grpc_health_v1.RegisterHealthServer(srv, hs)
	reflection.Register(srv)

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
	body := fmt.Sprintf(`{"ID":"mall-%s","Name":"rpc-mall","Address":"%s","Port":%s,"Check":{"Name":"mall gRPC","GRPC":"%s:%s","GRPCUseTLS":false,"Interval":"10s","Timeout":"3s","DeregisterCriticalServiceAfter":"90s"}}`, hostname, myIP, port, myIP, port)
	go func() { http.Post(consul+"/v1/agent/service/register", "application/json", strings.NewReader(body)) }()
}
