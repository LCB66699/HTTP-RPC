#include "otel_tracer.h"

#include <grpcpp/server_context.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/span_metadata.h>
#include <opentelemetry/trace/trace_id.h>
#include <opentelemetry/trace/trace_flags.h>
#include <opentelemetry/trace/provider.h>
#include <cstdio>
#include <cstring>

namespace context = opentelemetry::context;
namespace nostd = opentelemetry::nostd;
namespace sdktrace = opentelemetry::sdk::trace;
namespace resource = opentelemetry::sdk::resource;
namespace trace = opentelemetry::trace;

static nostd::shared_ptr<trace::Tracer> g_tracer;

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
    auto provider2 = trace::Provider::GetTracerProvider();
    g_tracer = provider2->GetTracer("http-rpc-cpp", "1.0.0");
    fprintf(stderr, "[OTel] Tracer initialized: service=%s\n", service_name.c_str());
}

// === Implementation of OTelSpan wrapper ===
class OTelSpanImpl : public OTelSpan {
   public:
    explicit OTelSpanImpl(nostd::shared_ptr<trace::Span> s) : span_(std::move(s)) {}
    ~OTelSpanImpl() override = default;
    void End() override { span_->End(); }
    bool IsRecording() const override { return span_->IsRecording(); }
    std::string TraceParent() const override {
        if (!span_->IsRecording()) return "";
        auto ctx = span_->GetContext();
        char buf[128];
        snprintf(buf, sizeof(buf), "00-%s-%s-%s",
                 HexTraceId(ctx.trace_id()).c_str(),
                 HexSpanId(ctx.span_id()).c_str(),
                 ctx.trace_flags().IsSampled() ? "01" : "00");
        return buf;
    }

   private:
    nostd::shared_ptr<trace::Span> span_;

    static std::string HexTraceId(const trace::TraceId &id) {
        char buf[33];
        id.ToLowerBase16(buf);
        buf[32] = 0;
        return buf;
    }
    static std::string HexSpanId(const trace::SpanId &id) {
        char buf[17];
        id.ToLowerBase16(buf);
        buf[16] = 0;
        return buf;
    }
};

std::shared_ptr<OTelSpan> StartSpan(const std::string &span_name) {
    if (!g_tracer) return nullptr;
    auto span = g_tracer->StartSpan(span_name);
    return std::make_shared<OTelSpanImpl>(span);
}

// Hex string to bytes
static bool HexToBytes(const std::string &hex, uint8_t *out, size_t out_len) {
    if (hex.size() != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        unsigned int byte;
        if (sscanf(hex.c_str() + i * 2, "%2x", &byte) != 1) return false;
        out[i] = static_cast<uint8_t>(byte);
    }
    return true;
}

std::shared_ptr<OTelSpan> StartSpanFromGRPC(void *ctx_ptr, const std::string &span_name) {
    if (!g_tracer || !ctx_ptr) return nullptr;
    auto *ctx = static_cast<grpc::ServerContext *>(ctx_ptr);
    const auto &md = ctx->client_metadata();

    auto it = md.find("traceparent");
    if (it == md.end()) {
        return std::make_shared<OTelSpanImpl>(g_tracer->StartSpan(span_name));
    }

    std::string tp(it->second.data(), it->second.length());
    if (tp.size() < 55 || tp[2] != '-' || tp[35] != '-' || tp[52] != '-') {
        return std::make_shared<OTelSpanImpl>(g_tracer->StartSpan(span_name));
    }

    uint8_t trace_id_bytes[16] = {0};
    uint8_t span_id_bytes[8] = {0};
    std::string trace_id_hex = tp.substr(3, 32);
    std::string span_id_hex = tp.substr(36, 16);
    std::string flags_hex = tp.substr(53, 2);
    if (!HexToBytes(trace_id_hex, trace_id_bytes, 16) || !HexToBytes(span_id_hex, span_id_bytes, 8)) {
        return std::make_shared<OTelSpanImpl>(g_tracer->StartSpan(span_name));
    }

    trace::SpanContext parent_ctx(
        trace::TraceId(trace_id_bytes),
        trace::SpanId(span_id_bytes),
        trace::TraceFlags(static_cast<uint8_t>(strtol(flags_hex.c_str(), nullptr, 16))),
        true);

    trace::StartSpanOptions opts;
    opts.parent = parent_ctx;
    return std::make_shared<OTelSpanImpl>(g_tracer->StartSpan(span_name, opts));
}

std::string GetCurrentTraceParent() {
    auto span = trace::Tracer::GetCurrentSpan();
    if (!span->IsRecording()) return "";
    auto ctx = span->GetContext();
    char tid_hex[33], sid_hex[17];
    ctx.trace_id().ToLowerBase16(tid_hex); tid_hex[32] = 0;
    ctx.span_id().ToLowerBase16(sid_hex); sid_hex[16] = 0;
    char buf[128];
    snprintf(buf, sizeof(buf), "00-%s-%s-%s",
             tid_hex, sid_hex, ctx.trace_flags().IsSampled() ? "01" : "00");
    return buf;
}
