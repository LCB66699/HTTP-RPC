#include "otel_tracer.h"

#include <grpcpp/server_context.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/propagation/http_text_format.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/span_metadata.h>
#include <opentelemetry/trace/trace_id.h>
#include <opentelemetry/trace/trace_flags.h>
#include <cstdio>
#include <cstring>

namespace context = opentelemetry::context;
namespace nostd = opentelemetry::nostd;
namespace sdktrace = opentelemetry::sdk::trace;
namespace resource = opentelemetry::sdk::resource;
namespace trace = opentelemetry::trace;

static const char *kEndpoint = "jaeger:4317";

void InitTracer(const std::string &service_name) {
    auto exporter = opentelemetry::exporter::otlp::OtlpGrpcExporterFactory::Create();
    if (!exporter) {
        fprintf(stderr, "[OTel] Failed to create OTLP exporter\n");
        return;
    }
    auto processor = sdktrace::SimpleProcessorFactory::Create(std::move(exporter));
    auto res = resource::Resource::Create({{"service.name", service_name}});
    auto provider = sdktrace::TracerProviderFactory::Create(std::move(processor), res);
    trace::Provider::SetTracerProvider(std::move(provider));
    fprintf(stderr, "[OTel] Tracer initialized: service=%s endpoint=%s\n", service_name.c_str(), kEndpoint);
}

nostd::shared_ptr<trace::Tracer> GetTracer() {
    auto provider = trace::Provider::GetTracerProvider();
    return provider->GetTracer("http-rpc-cpp", "1.0.0");
}

// Parse hex string (len must be even) into byte array
static bool HexToBytes(const std::string &hex, uint8_t *out, size_t out_len) {
    if (hex.size() != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        unsigned int byte;
        if (sscanf(hex.c_str() + i * 2, "%2x", &byte) != 1) return false;
        out[i] = static_cast<uint8_t>(byte);
    }
    return true;
}

nostd::shared_ptr<trace::Span> StartSpanFromGRPC(grpc::ServerContext *ctx, const std::string &span_name) {
    auto tracer = GetTracer();
    const auto &md = ctx->client_metadata();

    // Look for traceparent in gRPC metadata
    auto it = md.find("traceparent");
    if (it == md.end()) {
        // No traceparent → start a new root span
        return tracer->StartSpan(span_name);
    }

    std::string tp(it->second.data(), it->second.length());
    // Format: 00-{trace_id_32hex}-{span_id_16hex}-{flags_2hex}
    if (tp.size() < 55 || tp[2] != '-' || tp[35] != '-' || tp[52] != '-') {
        return tracer->StartSpan(span_name);
    }

    std::string trace_id_hex = tp.substr(3, 32);
    std::string span_id_hex = tp.substr(36, 16);
    std::string flags_hex = tp.substr(53, 2);

    uint8_t trace_id_bytes[16] = {0};
    uint8_t span_id_bytes[8] = {0};
    if (!HexToBytes(trace_id_hex, trace_id_bytes, 16) || !HexToBytes(span_id_hex, span_id_bytes, 8)) {
        return tracer->StartSpan(span_name);
    }

    trace::TraceId trace_id(trace_id_bytes);
    trace::SpanId span_id(span_id_bytes);
    trace::TraceFlags flags(static_cast<uint8_t>(strtol(flags_hex.c_str(), nullptr, 16)));

    trace::SpanContext parent_ctx(trace_id, span_id, flags, true);

    trace::StartSpanOptions opts;
    opts.parent = parent_ctx;
    return tracer->StartSpan(span_name, opts);
}
