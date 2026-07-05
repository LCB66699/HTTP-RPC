package middleware

import (
	"context"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/prometheus/client_golang/prometheus"
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

// GinMetrics collects HTTP request metrics for Prometheus.
func GinMetrics() gin.HandlerFunc {
	return func(c *gin.Context) {
		c.Next()
		httpRequestsTotal.WithLabelValues(
			c.Request.Method, c.FullPath(), strconv.Itoa(c.Writer.Status()),
		).Inc()
	}
}

// GrpcMetricsInterceptor records gRPC call duration.
func GrpcMetricsInterceptor() grpc.UnaryClientInterceptor {
	return func(ctx context.Context, method string, req, reply any, cc *grpc.ClientConn, invoker grpc.UnaryInvoker, opts ...grpc.CallOption) error {
		start := time.Now()
		err := invoker(ctx, method, req, reply, cc, opts...)
		parts := strings.SplitN(strings.TrimPrefix(method, "/"), "/", 2)
		service, meth := "unknown", method
		if len(parts) >= 2 {
			service, meth = parts[0], parts[1]
		}
		grpcRequestDuration.WithLabelValues(service, meth).Observe(time.Since(start).Seconds())
		return err
	}
}
