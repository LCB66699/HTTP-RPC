package main

import (
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"google.golang.org/grpc"
)

var (
	httpRequestsTotal = prometheus.NewCounterVec(
		prometheus.CounterOpts{
			Name: "http_requests_total",
			Help: "Total HTTP requests by method, path and status.",
		},
		[]string{"method", "path", "status"},
	)
	grpcRequestDuration = prometheus.NewHistogramVec(
		prometheus.HistogramOpts{
			Name:    "grpc_request_duration_seconds",
			Help:    "gRPC call duration in seconds, by service and method.",
			Buckets: prometheus.DefBuckets,
		},
		[]string{"service", "method"},
	)
	circuitBreakerState = prometheus.NewGaugeVec(
		prometheus.GaugeOpts{
			Name: "circuit_breaker_state",
			Help: "Circuit breaker state: 0=Closed, 1=HalfOpen, 2=Open",
		},
		[]string{"name"},
	)
)

func init() {
	prometheus.MustRegister(httpRequestsTotal)
	prometheus.MustRegister(grpcRequestDuration)
	prometheus.MustRegister(circuitBreakerState)
}

func metricsHandler() http.Handler { return promhttp.Handler() }

// statusRecorder 拦截 WriteHeader 以捕获 HTTP 状态码
type statusRecorder struct {
	http.ResponseWriter
	status int
}

func (r *statusRecorder) WriteHeader(code int) {
	r.status = code
	r.ResponseWriter.WriteHeader(code)
}

func metricsMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		sr := &statusRecorder{ResponseWriter: w, status: 200}
		next.ServeHTTP(sr, r)
		// r.Pattern 避免参数化路由的基数爆炸（Go 1.22+）
		httpRequestsTotal.WithLabelValues(r.Method, r.Pattern, strconv.Itoa(sr.status)).Inc()
		_ = start
	})
}

// grpcMetricsInterceptor 记录每个 gRPC 调用的延迟
func grpcMetricsInterceptor() grpc.UnaryClientInterceptor {
	return func(method string, req, reply any, cc *grpc.ClientConn, invoker grpc.UnaryInvoker, opts ...grpc.CallOption) error {
		start := time.Now()
		err := invoker(method, req, reply, cc, opts...)
		parts := strings.SplitN(strings.TrimPrefix(method, "/"), "/", 2)
		service, meth := "unknown", method
		if len(parts) >= 2 {
			service, meth = parts[0], parts[1]
		}
		grpcRequestDuration.WithLabelValues(service, meth).Observe(time.Since(start).Seconds())
		return err
	}
}
