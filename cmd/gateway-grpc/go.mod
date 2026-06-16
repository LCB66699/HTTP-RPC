module github.com/lcb66699/http-rpc/gateway-grpc

go 1.22

require (
	github.com/grpc-ecosystem/grpc-gateway/v2 v2.23.0
	google.golang.org/grpc v1.64.0
	google.golang.org/protobuf v1.34.1
	github.com/golang-jwt/jwt/v5 v5.2.1
	github.com/rs/cors v1.11.0
	github.com/sony/gobreaker/v2 v2.1.0
	github.com/redis/go-redis/v9 v9.5.1
	go.opentelemetry.io/contrib/instrumentation/google.golang.org/grpc/otelgrpc v0.53.0
	go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp v0.53.0
	go.opentelemetry.io/otel v1.28.0
	go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracegrpc v1.28.0
	go.opentelemetry.io/otel/sdk v1.28.0
)

replace gateway-grpc/gen/rpc => ./gen/rpc
