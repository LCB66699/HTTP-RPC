// async_http — epoll + C++20 coroutine HTTP/1.1 server
#include "async_http.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cerrno>

namespace rpc {

// ---- helpers ----
static std::string BuildHttpResponse(const AsyncResponse& resp) {
    std::string r;
    r += "HTTP/1.1 " + std::to_string(resp.status) + " OK\r\n";
    r += "Content-Type: " + resp.content_type + "\r\n";
    r += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
    for (auto& [k, v] : resp.headers) r += k + ": " + v + "\r\n";
    r += "Connection: close\r\n\r\n";
    r += resp.body;
    return r;
}

// ---- 路由 ----
void AsyncHttpServer::Get(std::string p, AsyncHandler h)  { routes_.push_back({"GET",std::move(p),std::move(h)}); }
void AsyncHttpServer::Post(std::string p, AsyncHandler h) { routes_.push_back({"POST",std::move(p),std::move(h)}); }
void AsyncHttpServer::Put(std::string p, AsyncHandler h)  { routes_.push_back({"PUT",std::move(p),std::move(h)}); }
void AsyncHttpServer::Options(std::string) {}

AsyncHandler* AsyncHttpServer::MatchRoute(std::string_view m, std::string_view p) {
    for (auto& r : routes_) if (r.method == m && r.path == p) return &r.handler;
    return nullptr;
}

// ---- 构造: 创建监听 socket + epoll ----
AsyncHttpServer::AsyncHttpServer(int port) : port_(port) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(port);
    bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd_, SOMAXCONN);
    epoll_fd_ = epoll_create1(0);
    struct epoll_event ev{};
    ev.events = EPOLLIN; ev.data.fd = listen_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev);
    printf("[async_http] 0.0.0.0:%d (epoll + coroutine, no keep-alive)\n", port);
}

AsyncHttpServer::~AsyncHttpServer() {
    if (listen_fd_ >= 0) close(listen_fd_);
    if (epoll_fd_ >= 0) close(epoll_fd_);
}

// ---- HTTP/1.1 请求解析 ----
struct ParsedRequest {
    AsyncRequest req;
    bool complete = false;
};

static bool TryParse(const std::string& buf, size_t& header_end, size_t& content_len, AsyncRequest& req) {
    if (header_end == 0) {
        auto pos = buf.find("\r\n\r\n");
        if (pos == std::string::npos) return false;
        header_end = pos + 4;
    }
    std::string_view hdr(buf.data(), header_end);
    auto le = hdr.find("\r\n");
    if (le == std::string::npos) return false;

    // 请求行
    std::string_view rl(buf.data(), le);
    auto s1 = rl.find(' '), s2 = rl.find(' ', s1 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos) return false;
    req.method = rl.substr(0, s1);
    req.path   = rl.substr(s1 + 1, s2 - s1 - 1);

    // Headers
    size_t pos = le + 2;
    while (pos < header_end) {
        auto nl = hdr.find("\r\n", pos);
        if (nl == std::string::npos) break;
        std::string_view l(buf.data() + pos, nl - pos);
        auto c = l.find(':');
        if (c != std::string::npos) {
            std::string k(l.substr(0, c));
            size_t vs = c + 1; while (vs < l.size() && l[vs] == ' ') vs++;
            std::string v(l.substr(vs));
            for (auto& ch : k) if (ch >= 'A' && ch <= 'Z') ch += 32;
            req.headers[std::move(k)] = std::move(v);
        }
        pos = nl + 2;
    }
    auto cl = req.headers.find("content-length");
    content_len = cl != req.headers.end() ? std::stoull(cl->second) : 0;
    return buf.size() >= header_end + content_len;
}

// ---- 请求处理协程 ----
static Task<> HandleOne(int fd, std::string read_buf, AsyncHttpServer* svr) {
    size_t header_end = 0, content_len = 0;
    AsyncRequest req;
    AsyncResponse res;

    if (!TryParse(read_buf, header_end, content_len, req)) {
        res.status = 400;
        res.set_content("{\"error\":\"bad request\"}");
    } else {
        req.body = read_buf.substr(header_end, content_len);

        // 路径去查询串
        std::string path(req.path);
        auto q = path.find('?');
        if (q != std::string::npos) path.resize(q);

        auto* handler = svr->MatchRoute(req.method, path);
        if (!handler) handler = svr->MatchRoute(req.method, req.path);

        if (handler) {
            co_await (*handler)(req, res);
        } else {
            res.status = 404;
            res.set_content("{\"error\":\"not found\"}");
        }
    }

    auto http_resp = BuildHttpResponse(res);
    // 写响应 (阻塞, 但数据小, 在 CQ 线程上执行无影响)
    size_t written = 0;
    while (written < http_resp.size()) {
        ssize_t n = write(fd, http_resp.data() + written, http_resp.size() - written);
        if (n <= 0) break;
        written += n;
    }
    close(fd);
}

// ---- 事件循环 ----
void AsyncHttpServer::Listen() {
    constexpr int MAX_EV = 1024;
    struct epoll_event evs[MAX_EV];

    while (true) {
        int n = epoll_wait(epoll_fd_, evs, MAX_EV, -1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) break;

        for (int i = 0; i < n; ++i) {
            if (evs[i].data.fd == listen_fd_) {
                // Accept 新连接
                while (true) {
                    struct sockaddr_in ca; socklen_t cl = sizeof(ca);
                    int cfd = accept4(listen_fd_, (struct sockaddr*)&ca, &cl, SOCK_NONBLOCK);
                    if (cfd < 0) break;
                    int opt = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                    AddClientFd(cfd);
                }
                continue;
            }

            int cfd = evs[i].data.fd;
            // 读客户端数据
            char tmp[8192];
            auto& buf = read_bufs_[cfd];
            while (true) {
                ssize_t rn = read(cfd, tmp, sizeof(tmp));
                if (rn > 0) { buf.append(tmp, rn); continue; }
                if (rn == 0) goto close_fd;  // EOF
                break;  // EAGAIN
            }

            // 尝试解析
            {
                size_t hdr_end = 0, cl = 0;
                AsyncRequest dummy;
                if (TryParse(buf, hdr_end, cl, dummy)) {
                    // 完整请求已收到 → 移出 epoll, 启动协程
                    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, cfd, nullptr);
                    read_bufs_.erase(cfd);
                    LaunchFireForget(HandleOne(cfd, std::move(buf), this));
                }
            }
            continue;

        close_fd:
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, cfd, nullptr);
            read_bufs_.erase(cfd);
            close(cfd);
        }
    }
}

void AsyncHttpServer::AddClientFd(int fd) {
    read_bufs_[fd]; // 插入空 buffer
    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
}

} // namespace rpc
