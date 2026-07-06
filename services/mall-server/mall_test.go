package main

import (
	"context"
	"testing"
	"time"

	pb "gateway-grpc/gen/rpc"
	"github.com/alicebob/miniredis/v2"
	"github.com/redis/go-redis/v9"
)

func setupMallServer(t *testing.T) (*mallServer, *miniredis.Miniredis) {
	t.Helper()
	mr := miniredis.RunT(t)
	rdb := redis.NewClient(&redis.Options{Addr: mr.Addr()})
	srv := &mallServer{rdb: rdb}
	srv.initSampleData()
	return srv, mr
}

// ---- Products ----

func TestListProducts(t *testing.T) {
	srv, _ := setupMallServer(t)
	resp, err := srv.ListProducts(context.Background(), &pb.ListProductsRequest{})
	if err != nil {
		t.Fatal(err)
	}
	if !resp.Success {
		t.Fatal("expected success")
	}
	if len(resp.Products) == 0 {
		t.Error("should have sample products")
	}
}

func TestGetProduct(t *testing.T) {
	srv, _ := setupMallServer(t)
	resp, err := srv.GetProduct(context.Background(), &pb.GetProductRequest{Id: 1})
	if err != nil {
		t.Fatal(err)
	}
	if !resp.Success {
		t.Fatal("product 1 should exist")
	}
	if resp.Product.Name == "" {
		t.Error("product must have a name")
	}
}

func TestGetProductNotFound(t *testing.T) {
	srv, _ := setupMallServer(t)
	resp, err := srv.GetProduct(context.Background(), &pb.GetProductRequest{Id: 99999})
	if err != nil {
		t.Fatal(err)
	}
	if resp.Success {
		t.Error("product 99999 should not exist")
	}
}

// ---- Seckill ----

func TestCreateSeckill(t *testing.T) {
	srv, _ := setupMallServer(t)
	now := time.Now().Unix()
	resp, err := srv.CreateSeckill(context.Background(), &pb.CreateSeckillRequest{
		ProductId: 1, SeckillPrice: 50, SeckillStock: 100,
		StartAt: now + 10, EndAt: now + 3600,
	})
	if err != nil {
		t.Fatal(err)
	}
	if !resp.Success {
		t.Fatalf("create seckill failed: %s", resp.Error)
	}
	if resp.Seckill.SeckillStock != 100 {
		t.Errorf("stock = %d, want 100", resp.Seckill.SeckillStock)
	}
}

func TestListSeckills(t *testing.T) {
	srv, _ := setupMallServer(t)
	now := time.Now().Unix()
	srv.CreateSeckill(context.Background(), &pb.CreateSeckillRequest{
		ProductId: 1, SeckillPrice: 50, SeckillStock: 10,
		StartAt: now + 10, EndAt: now + 3600,
	})

	resp, err := srv.ListSeckills(context.Background(), &pb.ListSeckillsRequest{})
	if err != nil {
		t.Fatal(err)
	}
	if !resp.Success {
		t.Fatal("expected success")
	}
	if len(resp.Seckills) == 0 {
		t.Error("should have at least 1 seckill")
	}
}

// ---- Seckill Order ----

func TestSeckillOrderSuccess(t *testing.T) {
	srv, _ := setupMallServer(t)
	ctx := context.Background()
	now := time.Now().Unix()

	// Create a seckill
	cr, _ := srv.CreateSeckill(ctx, &pb.CreateSeckillRequest{
		ProductId: 1, SeckillPrice: 50, SeckillStock: 10,
		StartAt: now - 10, EndAt: now + 3600, // active now
	})

	// Place an order
	resp, err := srv.SeckillOrder(ctx, &pb.SeckillOrderRequest{
		SeckillId: cr.Seckill.Id, IdempotencyKey: "order:1",
		UserId: 42,
	})
	if err != nil {
		t.Fatal(err)
	}
	if !resp.Success {
		t.Fatalf("order failed: %s", resp.Error)
	}
	if resp.Order.Amount != 50 {
		t.Errorf("amount = %d, want 50", resp.Order.Amount)
	}

	// Verify stock decreased
	stock, _ := srv.rdb.Get(ctx, "seckill:stock:1").Int64()
	if stock != 9 {
		t.Errorf("remaining stock = %d, want 9", stock)
	}
}

func TestSeckillOrderDuplicate(t *testing.T) {
	srv, _ := setupMallServer(t)
	ctx := context.Background()
	now := time.Now().Unix()

	cr, _ := srv.CreateSeckill(ctx, &pb.CreateSeckillRequest{
		ProductId: 1, SeckillPrice: 50, SeckillStock: 10,
		StartAt: now - 10, EndAt: now + 3600,
	})

	srv.SeckillOrder(ctx, &pb.SeckillOrderRequest{
		SeckillId: cr.Seckill.Id, IdempotencyKey: "dup:1", UserId: 42,
	})
	// Duplicate
	resp, _ := srv.SeckillOrder(ctx, &pb.SeckillOrderRequest{
		SeckillId: cr.Seckill.Id, IdempotencyKey: "dup:1", UserId: 42,
	})
	if resp.Success {
		t.Error("duplicate order should fail")
	}
}

func TestSeckillOrderSoldOut(t *testing.T) {
	srv, _ := setupMallServer(t)
	ctx := context.Background()
	now := time.Now().Unix()

	cr, _ := srv.CreateSeckill(ctx, &pb.CreateSeckillRequest{
		ProductId: 1, SeckillPrice: 10, SeckillStock: 2,
		StartAt: now - 10, EndAt: now + 3600,
	})

	// Buy all stock
	for i := 0; i < 2; i++ {
		resp, _ := srv.SeckillOrder(ctx, &pb.SeckillOrderRequest{
			SeckillId: cr.Seckill.Id,
			IdempotencyKey: fmt.Sprintf("order:sold:%d", i),
			UserId: int64(i),
		})
		if !resp.Success {
			t.Fatalf("order %d should succeed", i)
		}
	}
	// This should fail
	resp, _ := srv.SeckillOrder(ctx, &pb.SeckillOrderRequest{
		SeckillId: cr.Seckill.Id, IdempotencyKey: "order:sold:99", UserId: 99,
	})
	if resp.Success {
		t.Error("order should fail when sold out")
	}
}

func TestSeckillOrderTimeWindow(t *testing.T) {
	srv, _ := setupMallServer(t)
	ctx := context.Background()
	now := time.Now().Unix()

	// Not started yet
	cr, _ := srv.CreateSeckill(ctx, &pb.CreateSeckillRequest{
		ProductId: 1, SeckillPrice: 10, SeckillStock: 10,
		StartAt: now + 3600, EndAt: now + 7200,
	})
	resp, _ := srv.SeckillOrder(ctx, &pb.SeckillOrderRequest{
		SeckillId: cr.Seckill.Id, IdempotencyKey: "order:future", UserId: 42,
	})
	if resp.Success {
		t.Error("order should fail before seckill starts")
	}

	// Already ended
	cr2, _ := srv.CreateSeckill(ctx, &pb.CreateSeckillRequest{
		ProductId: 1, SeckillPrice: 10, SeckillStock: 10,
		StartAt: now - 7200, EndAt: now - 3600,
	})
	resp2, _ := srv.SeckillOrder(ctx, &pb.SeckillOrderRequest{
		SeckillId: cr2.Seckill.Id, IdempotencyKey: "order:past", UserId: 42,
	})
	if resp2.Success {
		t.Error("order should fail after seckill ended")
	}
}

func TestListOrders(t *testing.T) {
	srv, _ := setupMallServer(t)
	ctx := context.Background()
	now := time.Now().Unix()

	cr, _ := srv.CreateSeckill(ctx, &pb.CreateSeckillRequest{
		ProductId: 1, SeckillPrice: 50, SeckillStock: 10,
		StartAt: now - 10, EndAt: now + 3600,
	})
	srv.SeckillOrder(ctx, &pb.SeckillOrderRequest{
		SeckillId: cr.Seckill.Id, IdempotencyKey: "order:list:1", UserId: 42,
	})

	resp, err := srv.ListOrders(ctx, &pb.ListOrdersRequest{UserId: 42})
	if err != nil {
		t.Fatal(err)
	}
	if !resp.Success {
		t.Fatal("expected success")
	}
	if len(resp.Orders) == 0 {
		t.Error("should have at least 1 order")
	}
}
