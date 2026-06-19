package main

import (
	"net/http"
	"strconv"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
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

// metricsHandler exposes Prometheus /metrics endpoint.
func metricsHandler() http.Handler { return promhttp.Handler() }

// metricsMiddleware records HTTP request counts.
func metricsMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		httpRequestsTotal.WithLabelValues(r.Method, r.URL.Path, strconv.Itoa(0)).Inc()
		_ = start // reserved for latency histogram
	})
}
