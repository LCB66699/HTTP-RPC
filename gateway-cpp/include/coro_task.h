// ============================================================
// coro_task — 轻量 C++20 协程 Task<T>
// ============================================================
#pragma once
#include <coroutine>
#include <exception>
#include <variant>
#include <optional>

template<typename T = void>
struct Task {
    struct promise_type {
        std::variant<std::monostate, T, std::exception_ptr> result;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                // 协程结束，销毁 frame（由外部管理生命周期时需定制）
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(T val) { result.template emplace<1>(std::move(val)); }
        void unhandled_exception() { result.template emplace<2>(std::current_exception()); }
    };

    std::coroutine_handle<promise_type> handle;

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~Task() { if (handle && !handle.done()) handle.destroy(); }
    Task(Task&& o) noexcept : handle(std::exchange(o.handle, nullptr)) {}
    Task& operator=(Task&& o) noexcept {
        if (handle && !handle.done()) handle.destroy();
        handle = std::exchange(o.handle, nullptr);
        return *this;
    }
    Task(const Task&) = delete;

    T get() {
        if (auto* p = std::get_if<2>(&handle.promise().result))
            std::rethrow_exception(*p);
        return std::move(std::get<1>(handle.promise().result));
    }
};

template<>
struct Task<void> {
    struct promise_type {
        std::exception_ptr err;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { err = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle;

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    ~Task() { if (handle && !handle.done()) handle.destroy(); }
    Task(Task&& o) noexcept : handle(std::exchange(o.handle, nullptr)) {}
    Task& operator=(Task&& o) noexcept {
        if (handle && !handle.done()) handle.destroy();
        handle = std::exchange(o.handle, nullptr);
        return *this;
    }
    Task(const Task&) = delete;

    void get() {
        if (handle.promise().err) std::rethrow_exception(handle.promise().err);
    }
};
