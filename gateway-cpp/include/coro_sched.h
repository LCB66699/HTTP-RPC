// ============================================================
// coro_sched — 协程调度（fire-and-forget）
// ============================================================
#pragma once
#include "coro_task.h"
#include <coroutine>

// ---- 启动协程，不等待完成 ----
// 用法: LaunchFireForget(SomeAsyncHandler(req, res));
// 协程在 co_await 处挂起，CQ 线程后续恢复，
// 完成时 self-destroy（final_suspend = suspend_never）
template<typename T>
inline void LaunchFireForget(Task<T>&& t) {
    // 取出 handle，重置 Task，防止析构时 destroy
    auto h = std::exchange(t.handle, nullptr);
    (void)h;
    // 协程已挂起在 co_await gRPC 上
    // CQ 线程会 resume 它，届时 self-destroy
}
