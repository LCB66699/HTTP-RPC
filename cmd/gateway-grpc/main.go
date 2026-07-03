package main

import (
	"context"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/redis/go-redis/v9"
	"github.com/grpc-ecosystem/grpc-gateway/v2/runtime"
	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
	"google.golang.org/grpc/metadata"
	"google.golang.org/protobuf/encoding/protojson"

	gw "github.com/lcb66699/http-rpc/gateway-grpc/internal/gateway"
	"go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc"
	"go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracegrpc"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.24.0"
	"google.golang.org/grpc"
	"google.golang.org/grpc/connectivity"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/keepalive"

	pb "gateway-grpc/gen/rpc"
)

var authConn, sheetConn, fileConn *grpc.ClientConn

func initTracer() (*sdktrace.TracerProvider, error) {
	ctx := context.Background()
	endpoint := getenv("OTEL_EXPORTER_OTLP_ENDPOINT", "jaeger:4317")
	exporter, err := otlptracegrpc.New(ctx,
		otlptracegrpc.WithEndpoint(endpoint),
		otlptracegrpc.WithInsecure(),
	)
	if err != nil {
		log.Printf("[tracing] OTLP exporter error: %v", err)
		return nil, err
	}
	tp := sdktrace.NewTracerProvider(
		sdktrace.WithBatcher(exporter),
		sdktrace.WithSampler(sdktrace.AlwaysSample()),
		sdktrace.WithResource(resource.NewWithAttributes(
			semconv.SchemaURL,
			semconv.ServiceName("gateway-grpc"),
		)),
	)
	otel.SetTracerProvider(tp)
	otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(
		propagation.TraceContext{},
		propagation.Baggage{},
	))
	log.Printf("[tracing] initialized, endpoint=%s", endpoint)
	return tp, nil
}

func main() {
	tp, err := initTracer()
	if err != nil {
		log.Printf("[tracing] WARNING: tracer not available: %v", err)
	}
	if tp != nil {
		defer func() {
			if err := tp.Shutdown(context.Background()); err != nil {
				log.Printf("[tracing] shutdown error: %v", err)
			}
		}()
	}

	mux := http.NewServeMux()

	kp := grpc.WithKeepaliveParams(keepalive.ClientParameters{
		Time: 10 * time.Second, Timeout: 3 * time.Second,
	})
	creds := grpc.WithTransportCredentials(insecure.NewCredentials())
	lb := grpc.WithDefaultServiceConfig(`{"loadBalancingConfig":[{"round_robin":{}}]}`)
	otelStats := grpc.WithStatsHandler(otelgrpc.NewClientHandler())

	// Circuit breakers — created before gRPC connections so interceptors can
	// be registered. Each service gets its own breaker; the interceptor
	// applies circuit-breaking, retry, 3s timeout, and slow-call tagging
	// to every gRPC call on the connection.
	cbMetrics := func(name string, val float64) { circuitBreakerState.WithLabelValues(name).Set(val) }
	cbAuth := gw.NewCBSlow("auth", cbMetrics)
	cbSheet := gw.NewCBSlow("sheet", cbMetrics)
	cbFile := gw.NewCBSlow("file", cbMetrics)
	cbSearch := gw.NewCBSlow("search", cbMetrics)

	authAddr := getenv("AUTH_ADDR", "rpc-auth:50051")
	sheetAddr := getenv("SHEET_ADDR", "rpc-sheet:50051")
	fileAddr := getenv("FILE_ADDR", "rpc-file:50051")
	searchAddr := getenv("SEARCH_ADDR", "rpc-search:50051")
	log.Printf("Auth=%s Sheet=%s File=%s Search=%s", authAddr, sheetAddr, fileAddr, searchAddr)

	baseOpts := []grpc.DialOption{creds, kp, lb, otelStats}
	authConn, _ = grpc.NewClient("dns:///"+authAddr, append(baseOpts,
		grpc.WithChainUnaryInterceptor(grpcMetricsInterceptor(), cbAuth.Interceptor()))...)
	sheetConn, _ = grpc.NewClient("dns:///"+sheetAddr, append(baseOpts,
		grpc.WithChainUnaryInterceptor(grpcMetricsInterceptor(), cbSheet.Interceptor()))...)
	fileConn, _ = grpc.NewClient("dns:///"+fileAddr, append(baseOpts,
		grpc.WithChainUnaryInterceptor(grpcMetricsInterceptor(), cbFile.Interceptor()))...)
	searchConn, _ := grpc.NewClient("dns:///"+searchAddr, append(baseOpts,
		grpc.WithChainUnaryInterceptor(grpcMetricsInterceptor(), cbSearch.Interceptor()))...)

	redisAddr := getenv("REDIS_ADDR", "redis-cluster-7000:7000")
	redisPass := getenv("REDIS_PASSWORD", "rpc-redis-123456")
	rdb := redis.NewClient(&redis.Options{Addr: redisAddr, Password: redisPass})

	gwInst := &gw.Gateway{
		AuthClient:   pb.NewAuthServiceClient(authConn),
		SheetClient:  pb.NewSpreadsheetServiceClient(sheetConn),
		FileClient:   pb.NewFileServiceClient(fileConn),
		SearchClient: pb.NewSearchServiceClient(searchConn),
		SharedClient: pb.NewSharingServiceClient(authConn),
		RDB:          rdb,
		CBSearch:     cbSearch,
		CBSheet:      cbSheet,
		CBFile:       cbFile,
	}

	// ---- gRPC-gateway mux (replaces SpreadsheetService CRUD handlers) ----
	gwmux := runtime.NewServeMux(
		runtime.WithMetadata(func(ctx context.Context, r *http.Request) metadata.MD {
			for _, c := range r.Cookies() {
				if c.Name == "rpc_at" {
					return metadata.Pairs("authorization", "Bearer "+c.Value)
				}
			}
			return nil
		}),
		runtime.WithMarshalerOption(runtime.MIMEWildcard, &runtime.JSONPb{
			MarshalOptions: protojson.MarshalOptions{
				UseProtoNames:   true,
				EmitUnpopulated: true,
			},
			UnmarshalOptions: protojson.UnmarshalOptions{
				DiscardUnknown: true,
			},
		}),
	)
	{
		ctx := context.Background()
		if err := pb.RegisterSpreadsheetServiceHandler(ctx, gwmux, sheetConn); err != nil {
			log.Fatalf("register spreadsheet gRPC-gateway: %v", err)
		}
	}

	mux.HandleFunc("POST /api/v1/register", gwInst.Register)
	mux.HandleFunc("POST /api/v1/login", gwInst.Login)
	mux.HandleFunc("POST /api/v1/refresh", gwInst.Refresh)
	mux.HandleFunc("PUT /api/v1/me/password", gwInst.ChangePassword)
	mux.HandleFunc("POST /api/v1/auth/otp/send", gwInst.OTPSend)
	mux.HandleFunc("POST /api/v1/auth/phone/login", gwInst.PhoneLogin)
	mux.Handle("GET /api/v1/metrics", metricsHandler())
	mux.HandleFunc("GET /api/v1/health", gwInst.Health)
	mux.HandleFunc("GET /api/v1/health/ready", func(w http.ResponseWriter, r *http.Request) {
		checkConn := func(name string, conn *grpc.ClientConn) string {
			s := conn.GetState()
			if s == connectivity.Ready {
				return "OK"
			}
			return s.String()
		}
		status := map[string]string{"gateway": "OK"}
		status["auth"] = checkConn("auth", authConn)
		status["sheet"] = checkConn("sheet", sheetConn)
		status["file"] = checkConn("file", fileConn)
		allOK := status["auth"] == "OK" && status["sheet"] == "OK" && status["file"] == "OK"
		code := http.StatusOK
		if !allOK {
			code = http.StatusServiceUnavailable
		}
		gw.WriteJSONStatus(w, code, map[string]interface{}{"_all": allOK, "status": status})
	})
	mux.HandleFunc("GET /api/v1/me", gwInst.Me)
	mux.HandleFunc("GET /api/v1/services", gwInst.Services)
	mux.HandleFunc("GET /api/v1/history", gwInst.History)

	// Search
	mux.HandleFunc("POST /api/v1/search", gwInst.Search)

	// Sharing
	mux.HandleFunc("POST /api/v1/sheets/{id}/share", gwInst.ShareSheet)
	mux.HandleFunc("DELETE /api/v1/sheets/{id}/share", gwInst.RevokeShare)
	mux.HandleFunc("GET /api/v1/sheets/{id}/share", gwInst.ListShares)
	mux.HandleFunc("POST /api/v1/sheets/{id}/share-link", gwInst.CreateShareLink)
	mux.HandleFunc("GET /api/v1/s/{token}", gwInst.ShareByToken)
	// File CRUD
	mux.HandleFunc("PUT /api/v1/files/{id}/move", gwInst.MoveFile)
	mux.HandleFunc("POST /api/v1/files/folder", gwInst.CreateFolder)
	mux.HandleFunc("GET /api/v1/files", gwInst.ListFiles)
	mux.HandleFunc("POST /api/v1/files/upload", gwInst.UploadFile)
	mux.HandleFunc("GET /api/v1/files/{id}", gwInst.GetFile)
	mux.HandleFunc("DELETE /api/v1/files/{id}", gwInst.DeleteFile)

	// ---- JWT verification middleware ----
	// The gateway verifies JWT signatures from the rpc_at cookie independently
	// (defense-in-depth). Even when Envoy injects X-Rpc-Uid at the edge,
	// the middleware re-verifies and overwrites, preventing header forgery.
	jwtSecret := getenv("JWT_SECRET", "")
	if jwtSecret == "" {
		log.Fatal("JWT_SECRET environment variable is required")
	}
	gw.SetJWTSecret(jwtSecret)

	// ---- Dispatcher: sheet CRUD -> gwmux, everything else -> custom mux ----
	top := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if strings.HasPrefix(r.URL.Path, "/api/v1/sheets") {
			rest := strings.TrimPrefix(r.URL.Path, "/api/v1/sheets")
			segments := strings.Split(strings.Trim(rest, "/"), "/")
			// CRUD: 0-1 segments after /api/v1/sheets (e.g. /sheets, /sheets/123)
			// Sharing: 2+ segments (e.g. /sheets/123/share)
			if len(segments) <= 2 {
				gwmux.ServeHTTP(w, r)
				return
			}
		}
		mux.ServeHTTP(w, r)
	})
	handler := gw.AuthMiddleware(metricsMiddleware(otelhttp.NewHandler(top, "gateway-grpc",
		otelhttp.WithTracerProvider(otel.GetTracerProvider()),
		otelhttp.WithPropagators(otel.GetTextMapPropagator()),
	)))

	port := getenv("PORT", "8080")
	srv := &http.Server{Addr: ":" + port, Handler: h2c.NewHandler(handler, &http2.Server{})}
	go func() {
		log.Printf("[Gateway-gRPC] Listening on :%s", port)
		if err := srv.ListenAndServe(); err != http.ErrServerClosed {
			log.Fatalf("listen: %v", err)
		}
	}()

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, syscall.SIGINT, syscall.SIGTERM)
	<-quit
	log.Println("Shutting down gracefully...")
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if err := srv.Shutdown(ctx); err != nil {
		log.Fatalf("shutdown: %v", err)
	}
	log.Println("Server stopped")
}

func getenv(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}
