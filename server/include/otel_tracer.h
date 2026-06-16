#pragma once
#include <memory>
#include <string>

// Minimal forward declarations — no OpenTelemetry header dependency.
// All OTEL types are hidden in otel_tracer.cpp.

class OTelSpan {
   public:
    virtual ~OTelSpan() = default;
    virtual void End() = 0;
    virtual bool IsRecording() const = 0;
    // Returns W3C traceparent string for propagation.
    virtual std::string TraceParent() const = 0;
};

// Initialize OTLP gRPC exporter + TracerProvider.
void InitTracer(const std::string &service_name);

// Create a child span from gRPC metadata (or a new root if no traceparent).
// Extracts "traceparent" from gRPC client_metadata.
std::shared_ptr<OTelSpan> StartSpanFromGRPC(
    void *grpc_server_context, const std::string &span_name);

// Create a plain child span.
std::shared_ptr<OTelSpan> StartSpan(const std::string &span_name);

// Get current W3C traceparent from the active span (empty if not recording).
std::string GetCurrentTraceParent();
