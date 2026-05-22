// ============================================================
// coro_grpc — co_await gRPC Unary 调用
//
// 用法:
//   auto r = co_await GrpcCallOf(g_cq, stub, &Stub::AsyncXxx, req);
//   if (r.st.ok()) { ... r.resp.xxx() ... }
// ============================================================
#pragma once
#include <grpcpp/grpcpp.h>
#include <coroutine>
#include <thread>
#include <memory>
#include <chrono>

// ---- 返回类型 ----
template<typename Resp>
struct GrpcResult {
    grpc::Status st;
    Resp resp;
};

// ---- CQ tag 基类 (CqLoop 通过它唤醒协程) ----
struct GrpcTag {
    std::coroutine_handle<> coro;
    virtual ~GrpcTag() = default;
    void Wake() { if (coro) coro.resume(); }
};

// ---- 单次 gRPC 调用 (堆分配 = CQ tag) ----
template<typename Resp, typename Stub, typename Req>
struct GrpcCallData : GrpcTag {
    grpc::ClientContext ctx;
    Resp resp;
    grpc::Status status;
    std::unique_ptr<grpc::ClientAsyncResponseReader<Resp>> responder;

    using PrepareFn = std::unique_ptr<grpc::ClientAsyncResponseReader<Resp>>(Stub::*)(
        grpc::ClientContext*, const Req&, grpc::CompletionQueue*);

    GrpcCallData(grpc::CompletionQueue* cq, Stub* stub, PrepareFn fn,
                 const Req& req, int deadline_sec) {
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(deadline_sec));
        responder = (stub->*fn)(&ctx, req, cq);
        responder->Finish(&resp, &status, static_cast<void*>(this));
    }
};

// ---- co_await 包装器 (栈上, 内持堆指针) ----
template<typename Resp, typename Stub, typename Req>
struct GrpcCall {
    GrpcCallData<Resp, Stub, Req>* data;

    using PrepareFn = typename GrpcCallData<Resp, Stub, Req>::PrepareFn;

    GrpcCall(grpc::CompletionQueue* cq, Stub* stub, PrepareFn fn,
             const Req& req, int deadline_sec = 5)
        : data(new GrpcCallData<Resp, Stub, Req>(cq, stub, fn, req, deadline_sec)) {}

    GrpcCall(const GrpcCall&) = delete;
    GrpcCall(GrpcCall&& o) noexcept : data(std::exchange(o.data, nullptr)) {}

    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) { data->coro = h; }
    GrpcResult<Resp> await_resume() {
        GrpcResult<Resp> r{std::move(data->status), std::move(data->resp)};
        delete data;
        return r;
    }
};

// ---- 工厂函数: 从成员函数指针自动推导 Stub/Req/Resp ----
template<typename Resp, typename Stub, typename Req>
GrpcCall<Resp, Stub, Req> GrpcCallOf(
    grpc::CompletionQueue* cq, Stub* stub,
    std::unique_ptr<grpc::ClientAsyncResponseReader<Resp>> (Stub::*fn)(
        grpc::ClientContext*, const Req&, grpc::CompletionQueue*),
    const Req& req, int deadline_sec = 5) {
    return GrpcCall<Resp, Stub, Req>(cq, stub, fn, req, deadline_sec);
}

// ---- CQ 事件循环线程 (一个实例, 供全网关共享) ----
class CqLoop {
public:
    void Start() {
        running_ = true;
        thread_ = std::thread([this] {
            void* tag; bool ok;
            while (running_ && cq_.Next(&tag, &ok)) {
                static_cast<GrpcTag*>(tag)->Wake();
            }
        });
    }
    void Shutdown() {
        running_ = false;
        cq_.Shutdown();
        if (thread_.joinable()) thread_.join();
    }
    grpc::CompletionQueue* cq() { return &cq_; }

private:
    grpc::CompletionQueue cq_;
    std::thread thread_;
    bool running_ = true;
};
