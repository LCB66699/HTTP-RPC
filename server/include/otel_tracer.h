#pragma once
#include <memory>
#include <string>

#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/tracer.h>
#include <opentelemetry/nostd/shared_ptr.h>

namespace trace = opentelemetry::trace;
namespace nostd = opentelemetry::nostd;

// Initialize OTLP gRPC exporter + TracerProvider.
void InitTracer(const std::string &service_name);

// Get a tracer instance for creating spans.
nostd::shared_ptr<trace::Tracer> GetTracer();

// Forward declaration
namespace grpc { class ServerContext; }

// Extract W3C traceparent from gRPC client_metadata and start a child span.
nostd::shared_ptr<trace::Span> StartSpanFromGRPC(
    grpc::ServerContext *ctx, const std::string &span_name);
