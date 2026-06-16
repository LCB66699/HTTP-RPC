#pragma once
#include <memory>
#include <string>

namespace trace = opentelemetry::trace;
namespace nostd = opentelemetry::nostd;

// Initialize OTLP gRPC exporter + TracerProvider.
// Call once at process start. Pass service name like "spreadsheet-service".
void InitTracer(const std::string &service_name);

// Get a tracer instance for creating spans.
nostd::shared_ptr<trace::Tracer> GetTracer();

// Extract W3C traceparent from gRPC client_metadata and start a child span.
// If no traceparent is present, starts a new root span.
class grpc::ServerContext;
nostd::shared_ptr<trace::Span> StartSpanFromGRPC(
    grpc::ServerContext *ctx, const std::string &span_name);
