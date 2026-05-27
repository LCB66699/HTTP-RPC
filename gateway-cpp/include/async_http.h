// ============================================================
// async_http — epoll 事件驱动 + C++20 协程 HTTP/1.1 服务器
//
// 架构:
//   - 单线程 epoll 负责 accept + 读请求
//   - 每请求启动一个协程, co_await gRPC 时不占线程
//   - gRPC 返回后 CQ 线程恢复协程, 直接写 socket + close
//   - 无 keep-alive (每个请求一个连接, nginx 侧复用连接池)
// ============================================================
#pragma once

#include "coro_task.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <vector>
#include <cstdint>

namespace rpc {

struct AsyncRequest {
    std::string method, path, body;
    std::unordered_map<std::string, std::string> headers;
    std::string_view get_header_value(std::string_view name) const {
        auto it = headers.find(std::string(name));
        return it != headers.end() ? std::string_view(it->second) : std::string_view{};
    }
};

struct AsyncResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    void set_content(std::string b, std::string ct = "application/json") {
        body = std::move(b); content_type = std::move(ct);
    }
    void set_header(std::string k, std::string v) {
        headers.emplace_back(std::move(k), std::move(v));
    }
};

using AsyncHandler = std::function<Task<>(const AsyncRequest&, AsyncResponse&)>;

class AsyncHttpServer {
public:
    explicit AsyncHttpServer(int port);
    ~AsyncHttpServer();

    void Get(std::string path, AsyncHandler h);
    void Post(std::string path, AsyncHandler h);
    void Put(std::string path, AsyncHandler h);
    void Options(std::string path);

    // 阻塞当前线程, 运行事件循环
    void Listen();

    // 给 HandleOne 协程用的路由匹配
    AsyncHandler* MatchRoute(std::string_view method, std::string_view path);

private:
    struct Route { std::string method, path; AsyncHandler handler; };
    int port_, epoll_fd_ = -1, listen_fd_ = -1;
    std::vector<Route> routes_;
    std::unordered_map<int, std::string> read_bufs_;  // fd → 读缓冲
    void AddClientFd(int fd);
};

} // namespace rpc
