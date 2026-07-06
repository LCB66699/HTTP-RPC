package main

import (
	"context"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/redis/go-redis/v9"
	pb "gateway-grpc/gen/rpc"
	"go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracegrpc"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.24.0"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/keepalive"

	"github.com/lcb66699/http-rpc/server/handler"
	"github.com/lcb66699/http-rpc/server/middleware"
	"github.com/lcb66699/http-rpc/server/router"
	"github.com/lcb66699/http-rpc/server/ws"

	// Register consul:// resolver for gRPC service discovery.
	_ "github.com/lcb66699/http-rpc/server/discovery"
)

func initTracer() (*sdktrace.TracerProvider, error) {
	ctx := context.Background()
	endpoint := getenv("OTEL_EXPORTER_OTLP_ENDPOINT", "jaeger:4317")
	exporter, err := otlptracegrpc.New(ctx,
		otlptracegrpc.WithEndpoint(endpoint),
		otlptracegrpc.WithInsecure(),
	)
	if err != nil {
		slog.Warn("tracing: OTLP exporter error", "error", err)
		return nil, err
	}
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(exporter),
		sdktrace.WithSampler(sdktrace.AlwaysSample()),
		sdktrace.WithResource(resource.NewWithAttributes(
			semconv.SchemaURL,
			semconv.ServiceName("gin-gateway"),
		)),
	)
	otel.SetTracerProvider(tp)
	otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(
		propagation.TraceContext{},
		propagation.Baggage{},
	))
	slog.Info("tracing initialized", "endpoint", endpoint)
	return tp, nil
}

func main() {
	tp, err := initTracer()
	if err != nil {
		slog.Warn("tracing: tracer not available", "error", err)
	}
	if tp != nil {
		defer tp.Shutdown(context.Background())
	}

	kp := grpc.WithKeepaliveParams(keepalive.ClientParameters{
		Time: 10 * time.Second, Timeout: 3 * time.Second,
	})
	creds := grpc.WithTransportCredentials(insecure.NewCredentials())
	lb := grpc.WithDefaultServiceConfig(`{"loadBalancingConfig":[{"round_robin":{}}],"healthCheckConfig":{"serviceName":""}}`)
	otelStats := grpc.WithStatsHandler(otelgrpc.NewClientHandler())

	cbAuth := middleware.NewCBSlow("auth", nil)
	cbSheet := middleware.NewCBSlow("sheet", nil)
	cbFile := middleware.NewCBSlow("file", nil)
	cbSearch := middleware.NewCBSlow("search", nil)

	authAddr := getenv("AUTH_ADDR", "rpc-auth")
	sheetAddr := getenv("SHEET_ADDR", "rpc-sheet")
	fileAddr := getenv("FILE_ADDR", "rpc-file")
	searchAddr := getenv("SEARCH_ADDR", "rpc-search")
	slog.Info("backend addresses (consul)", "auth", authAddr, "sheet", sheetAddr, "file", fileAddr, "search", searchAddr)

	authConn, _ := grpc.NewClient("consul:///"+authAddr,
		append([]grpc.DialOption{creds, kp, lb, otelStats},
			grpc.WithChainUnaryInterceptor(middleware.GrpcMetricsInterceptor(), cbAuth.Interceptor()))...)
	sheetConn, _ := grpc.NewClient("consul:///"+sheetAddr,
		append([]grpc.DialOption{creds, kp, lb, otelStats},
			grpc.WithChainUnaryInterceptor(middleware.GrpcMetricsInterceptor(), cbSheet.Interceptor()))...)
	fileConn, _ := grpc.NewClient("consul:///"+fileAddr,
		append([]grpc.DialOption{creds, kp, lb, otelStats},
			grpc.WithChainUnaryInterceptor(middleware.GrpcMetricsInterceptor(), cbFile.Interceptor()))...)
	searchConn, _ := grpc.NewClient("consul:///"+searchAddr,
		append([]grpc.DialOption{creds, kp, lb, otelStats},
			grpc.WithChainUnaryInterceptor(middleware.GrpcMetricsInterceptor(), cbSearch.Interceptor()))...)

	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	rdb := redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})

	hub := ws.NewHub(rdb)
	go hub.Run()

	jwtSecret := getenv("JWT_SECRET", "")
	if jwtSecret == "" {
		slog.Error("JWT_SECRET environment variable is required")
		os.Exit(1)
	}

	h := handler.Handlers{
		Auth:    pb.NewAuthServiceClient(authConn),
		Sheet:   pb.NewSpreadsheetServiceClient(sheetConn),
		File:    pb.NewFileServiceClient(fileConn),
		SearchClient: pb.NewSearchServiceClient(searchConn),
		Share:     pb.NewSharingServiceClient(authConn),
		Workspace: pb.NewWorkspaceServiceClient(authConn),
		RDB:       rdb,
		CBAuth: cbAuth, CBSearch: cbSearch, CBSheet: cbSheet, CBFile: cbFile,
		WSHub: hub, WS: &ws.Handler{Hub: hub},
		JWTSecret: jwtSecret,
	}

	r := router.Setup(&h, jwtSecret)
	srv := &http.Server{Addr: ":" + getenv("PORT", "8080"), Handler: r}

	go func() {
		slog.Info("gin gateway listening", "port", getenv("PORT", "8080"))
		if err := srv.ListenAndServe(); err != http.ErrServerClosed {
			slog.Error("listen failed", "error", err)
			os.Exit(1)
		}
	}()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit
	slog.Info("shutting down gracefully")
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	srv.Shutdown(ctx)
	slog.Info("server stopped")
}

func getenv(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
