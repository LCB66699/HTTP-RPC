// ============================================================
// Http2Server — 基于 nghttp2 C API 的轻量 HTTP/2 Server
// 线程模型: 主线程 accept + 每连接一线程(poll+非阻塞I/O) + 线程池异步handler
// 协议: h2c cleartext (prior knowledge 模式, nginx proxy_http_version 2.0)
// ============================================================
#include "http2_server.h"

#include <nghttp2/nghttp2.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <exception>
#include <sys/eventfd.h>
#include <poll.h>
#include <deque>

// ============================================================
// Response body source — 给 nghttp2 data provider 用
// ============================================================
struct BodySource {
    std::string data;
    size_t offset = 0;
};

// ============================================================
// Per-stream 上下文
// ============================================================
struct StreamCtx {
    int32_t stream_id;
    std::string method;
    std::string path;
    std::string query;
    std::multimap<std::string, std::string> headers;
    std::string body;
    // 响应体 data provider 需存活到流关闭
    std::unique_ptr<BodySource> resp_body;
};

// ---- nghttp2 回调 ----

static ssize_t send_callback(nghttp2_session*, const uint8_t* data,
                              size_t length, int, void* user_data) {
    auto* conn = static_cast<ConnCtx*>(user_data);
    ssize_t n = ::write(conn->fd, data, length);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return NGHTTP2_ERR_WOULDBLOCK;
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return n;
}

static int on_begin_headers_callback(nghttp2_session* session,
                                      const nghttp2_frame* frame,
                                      void* /*user_data*/) {
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;

    auto* s = new StreamCtx();
    s->stream_id = frame->hd.stream_id;
    nghttp2_session_set_stream_user_data(session, frame->hd.stream_id, s);
    return 0;
}

static int on_header_callback(nghttp2_session* session,
                               const nghttp2_frame* frame,
                               const uint8_t* name, size_t namelen,
                               const uint8_t* value, size_t valuelen,
                               uint8_t /*flags*/,
                               void* /*user_data*/) {
    if (frame->hd.type != NGHTTP2_HEADERS) return 0;

    auto* s = static_cast<StreamCtx*>(
        nghttp2_session_get_stream_user_data(session, frame->hd.stream_id));
    if (!s) return 0;

    std::string key(reinterpret_cast<const char*>(name), namelen);
    std::string val(reinterpret_cast<const char*>(value), valuelen);

    s->headers.insert({key, val});

    if (key == ":method")       s->method = val;
    else if (key == ":path") {
        // 分离 path 和 query
        auto qpos = val.find('?');
        if (qpos != std::string::npos) {
            s->path  = val.substr(0, qpos);
            s->query = val.substr(qpos + 1);
        } else {
            s->path = val;
        }
    }

    return 0;
}

static int on_data_chunk_recv_callback(nghttp2_session* session,
                                        uint8_t /*flags*/,
                                        int32_t stream_id,
                                        const uint8_t* data, size_t len,
                                        void* /*user_data*/) {
    auto* s = static_cast<StreamCtx*>(
        nghttp2_session_get_stream_user_data(session, stream_id));
    if (!s) return 0;

    s->body.append(reinterpret_cast<const char*>(data), len);
    return 0;
}

static ssize_t body_read_callback(nghttp2_session*, int32_t,
                                   uint8_t* buf, size_t length,
                                   uint32_t* data_flags,
                                   nghttp2_data_source* source,
                                   void* /*user_data*/) {
    auto* bs = static_cast<BodySource*>(source->ptr);
    size_t remaining = bs->data.size() - bs->offset;
    if (remaining == 0) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }
    size_t n = std::min(length, remaining);
    std::memcpy(buf, bs->data.data() + bs->offset, n);
    bs->offset += n;
    if (bs->offset >= bs->data.size()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return static_cast<ssize_t>(n);
}

static int on_frame_recv_callback(nghttp2_session* session,
                                   const nghttp2_frame* frame,
                                   void* user_data) {
    auto* conn = static_cast<ConnCtx*>(user_data);
    int32_t stream_id = frame->hd.stream_id;

    auto* s = static_cast<StreamCtx*>(
        nghttp2_session_get_stream_user_data(session, stream_id));
    if (!s) return 0;

    bool end_stream = false;
    if (frame->hd.type == NGHTTP2_HEADERS &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
        end_stream = true;
    if (frame->hd.type == NGHTTP2_DATA &&
        (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
        end_stream = true;

    if (!end_stream) return 0;

    // 拷贝 StreamCtx 数据后异步提交到线程池，不阻塞 nghttp2 事件循环
    Http2Server::process_request_async(conn->server, conn, s->stream_id,
                                       s->method, s->path, s->query,
                                       s->headers, s->body);

    return 0;
}

static int on_stream_close_callback(nghttp2_session* session,
                                     int32_t stream_id,
                                     uint32_t /*error_code*/,
                                     void* /*user_data*/) {
    auto* s = static_cast<StreamCtx*>(
        nghttp2_session_get_stream_user_data(session, stream_id));
    delete s;
    nghttp2_session_set_stream_user_data(session, stream_id, nullptr);
    return 0;
}

// ---- Worker 线程：处理请求（阻塞 gRPC 等操作在这里执行） ----
void Http2Server::process_request_async(Http2Server* server, ConnCtx* conn,
                                        int32_t stream_id,
                                        std::string method, std::string path,
                                        std::string query,
                                        std::multimap<std::string, std::string> headers,
                                        std::string body) {
    server->thread_pool_->Submit([server, conn, stream_id,
                                   method = std::move(method),
                                   path = std::move(path),
                                   query = std::move(query),
                                   headers = std::move(headers),
                                   body = std::move(body)]() mutable {
        Request req;
        req.method = std::move(method);
        req.path = std::move(path);
        req.query_string = std::move(query);
        req.body = std::move(body);
        req.headers = std::move(headers);

        std::string ct = req.get_header_value("content-type");
        if (!ct.empty() && ct.find("multipart/form-data") != std::string::npos) {
            req.form.parse(ct, req.body);
        }

        Response res;
        res.status = 200;

        try {
            if (server->pre_handler_) {
                auto result = server->pre_handler_(req, res);
                if (result == HandlerResponse::Handled) {
                    goto enqueue;
                }
            }

            for (auto& route : server->routes_) {
                std::smatch match;
                if (req.method != route.method) continue;

                if (std::regex_match(req.path, match, route.pattern)) {
                    req.matches.clear();
                    for (size_t i = 1; i < match.size(); ++i)
                        req.matches.push_back(match[i].str());

                    route.handler(req, res);
                    goto enqueue;
                }
            }

            res.status = 404;
            res.set_content("Not Found", "text/plain");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("Internal Server Error: ") + e.what(), "text/plain");
        } catch (...) {
            res.status = 500;
            res.set_content("Internal Server Error", "text/plain");
        }

    enqueue:
        {
            PendingResponse pr;
            pr.stream_id = stream_id;
            pr.status = res.status;
            pr.body = std::move(res.body_);
            pr.content_type = std::move(res.content_type_);
            pr.headers = std::move(res.headers_);

            std::lock_guard<std::mutex> lock(conn->response_mutex);
            conn->pending_responses.push_back(std::move(pr));
        }

        uint64_t val = 1;
        (void)::write(conn->event_fd, &val, sizeof(val));
    });
}

// ---- nghttp2 事件线程：提交 worker 完成的响应 ----
void Http2Server::submit_pending_responses(nghttp2_session* session, ConnCtx* conn) {
    std::deque<PendingResponse> ready;
    {
        std::lock_guard<std::mutex> lock(conn->response_mutex);
        ready.swap(conn->pending_responses);
    }

    for (auto& pr : ready) {
        struct StreamCtx* s = static_cast<StreamCtx*>(
            nghttp2_session_get_stream_user_data(session, pr.stream_id));
        if (!s) {
            s = new StreamCtx();
            s->stream_id = pr.stream_id;
            nghttp2_session_set_stream_user_data(session, pr.stream_id, s);
        }

        std::string status_str = std::to_string(pr.status);
        std::vector<nghttp2_nv> nva;
        nva.push_back({(uint8_t*)":status", (uint8_t*)status_str.c_str(),
                       2, status_str.size(), NGHTTP2_NV_FLAG_NONE});

        if (!pr.content_type.empty()) {
            nva.push_back({(uint8_t*)"content-type",
                           (uint8_t*)pr.content_type.c_str(),
                           12, pr.content_type.size(), NGHTTP2_NV_FLAG_NONE});
        }

        std::vector<std::string> extra_hdr_storage;
        for (auto& kv : pr.headers) {
            extra_hdr_storage.push_back(kv.first);
            extra_hdr_storage.push_back(kv.second);
        }
        for (size_t i = 0; i < extra_hdr_storage.size(); i += 2) {
            nva.push_back({(uint8_t*)extra_hdr_storage[i].c_str(),
                           (uint8_t*)extra_hdr_storage[i+1].c_str(),
                           extra_hdr_storage[i].size(),
                           extra_hdr_storage[i+1].size(),
                           NGHTTP2_NV_FLAG_NONE});
        }

        s->resp_body = std::make_unique<BodySource>();
        s->resp_body->data = std::move(pr.body);

        nghttp2_data_provider data_prd;
        data_prd.source.ptr = s->resp_body.get();
        data_prd.read_callback = body_read_callback;

        nghttp2_submit_response(session, pr.stream_id, nva.data(), nva.size(), &data_prd);
    }
}

// ---- 连接处理线程 (poll + 非阻塞 I/O) ----
void Http2Server::handle_connection(int fd, Http2Server* self) {
    int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0) {
        ::close(fd);
        return;
    }

    // 设置 socket 非阻塞
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    ConnCtx conn;
    conn.fd = fd;
    conn.event_fd = event_fd;
    conn.server = self;

    nghttp2_session_callbacks* callbacks;
    nghttp2_session_callbacks_new(&callbacks);

    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(
        callbacks, on_begin_headers_callback);
    nghttp2_session_callbacks_set_on_header_callback(
        callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
        callbacks, on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(
        callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(
        callbacks, on_stream_close_callback);

    nghttp2_session* session;
    nghttp2_session_server_new(&session, callbacks, &conn);
    nghttp2_session_callbacks_del(callbacks);

    nghttp2_settings_entry iv[1] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 128}
    };
    nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 1);

    uint8_t buf[65536];
    struct pollfd fds[2];
    fds[0].fd = fd;
    fds[1].fd = event_fd;

    while (self->running_.load(std::memory_order_acquire)) {
        // 发送待发数据
        int rv = nghttp2_session_send(session);
        if (rv != 0 && rv != NGHTTP2_ERR_WOULDBLOCK) break;

        // 处理 worker 线程完成的响应
        submit_pending_responses(session, &conn);

        // 再次发送（提交的响应数据）
        rv = nghttp2_session_send(session);
        if (rv != 0 && rv != NGHTTP2_ERR_WOULDBLOCK) break;

        // 判断是否需要继续
        bool has_pending;
        {
            std::lock_guard<std::mutex> lock(conn.response_mutex);
            has_pending = !conn.pending_responses.empty();
        }
        if (!nghttp2_session_want_read(session) &&
            !nghttp2_session_want_write(session) && !has_pending) {
            break;
        }

        fds[0].events = POLLIN;
        fds[1].events = POLLIN;
        if (nghttp2_session_want_write(session)) fds[0].events |= POLLOUT;

        int poll_rv = poll(fds, 2, -1);
        if (poll_rv < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // eventfd 被 worker 线程唤醒
        if (fds[1].revents & POLLIN) {
            uint64_t val;
            (void)::read(event_fd, &val, sizeof(val));
        }

        // socket 可读
        if (fds[0].revents & POLLIN) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) break;
            } else if (n == 0) {
                break;  // 客户端关闭
            } else {
                nghttp2_session_mem_recv(session, buf, static_cast<size_t>(n));
            }
        }

        if (fds[0].revents & (POLLERR | POLLHUP)) break;
    }

    // 发送 GOAWAY
    nghttp2_session_terminate_session(session, NGHTTP2_NO_ERROR);
    nghttp2_session_send(session);

    nghttp2_session_del(session);
    ::close(fd);
    ::close(event_fd);
}

// ============================================================
// Multipart Form 解析（与之前实现相同）
// ============================================================
void Http2Server::Request::Form::parse(const std::string& content_type_header,
                                        const std::string& body) {
    std::string boundary;
    const std::string kBoundary = "boundary=";
    auto pos = content_type_header.find(kBoundary);
    if (pos == std::string::npos) return;
    boundary = content_type_header.substr(pos + kBoundary.size());
    if (!boundary.empty() && boundary.front() == '"') {
        boundary = boundary.substr(1);
        auto eq = boundary.find('"');
        if (eq != std::string::npos) boundary = boundary.substr(0, eq);
    }
    if (boundary.empty() || body.empty()) return;

    std::string delim = "--" + boundary;
    size_t start = body.find(delim);
    if (start == std::string::npos) return;
    start += delim.size();

    while (start < body.size()) {
        if (body.compare(start, 2, "\r\n") == 0) start += 2;
        if (body.compare(start, 2, "--") == 0) break;
        size_t end = body.find(delim, start);
        if (end == std::string::npos) break;

        std::string part = body.substr(start, end - start);
        while (!part.empty() && (part.back() == '\r' || part.back() == '\n'))
            part.pop_back();

        size_t header_end = part.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            start = end + delim.size();
            continue;
        }

        std::string hdrs = part.substr(0, header_end);
        std::string content = part.substr(header_end + 4);

        std::string name, filename;
        auto dp = hdrs.find("Content-Disposition:");
        if (dp != std::string::npos) {
            std::string disp = hdrs.substr(dp + 19);
            auto np = disp.find("name=\"");
            if (np != std::string::npos) {
                np += 6;
                auto ne = disp.find('"', np);
                if (ne != std::string::npos) name = disp.substr(np, ne - np);
            }
            auto fp = disp.find("filename=\"");
            if (fp != std::string::npos) {
                fp += 10;
                auto fe = disp.find('"', fp);
                if (fe != std::string::npos) filename = disp.substr(fp, fe - fp);
            }
        }

        if (!name.empty() && !filename.empty()) {
            FormFile f;
            f.filename = filename;
            f.content = content;
            auto cp = hdrs.find("Content-Type:");
            if (cp != std::string::npos) {
                cp += 13;
                while (cp < hdrs.size() && hdrs[cp] == ' ') cp++;
                auto ce = hdrs.find('\r', cp);
                if (ce == std::string::npos) ce = hdrs.size();
                f.content_type = hdrs.substr(cp, ce - cp);
            }
            files_[name] = std::move(f);
        }
        start = end + delim.size();
    }
}

Http2Server::Request::FormFile
Http2Server::Request::Form::get_file(const std::string& name) const {
    auto it = files_.find(name);
    if (it != files_.end()) return it->second;
    return {};
}

// ============================================================
// Request / Response 简单方法
// ============================================================
std::string Http2Server::Request::get_header_value(const std::string& key) const {
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (auto& h : headers) {
        std::string hk = h.first;
        std::transform(hk.begin(), hk.end(), hk.begin(), ::tolower);
        if (hk == lower) return h.second;
    }
    return "";
}

std::string Http2Server::Request::get_param_value(const std::string& key) const {
    if (query_string.empty()) return "";
    std::string pat = key + "=";
    auto pos = query_string.find(pat);
    if (pos == std::string::npos) return "";
    pos += pat.size();
    auto end = query_string.find('&', pos);
    if (end == std::string::npos) return query_string.substr(pos);
    return query_string.substr(pos, end - pos);
}

void Http2Server::Response::set_content(const std::string& body,
                                         const std::string& ct) {
    body_ = body;
    content_type_ = ct;
}

void Http2Server::Response::set_header(const std::string& key,
                                        const std::string& value) {
    headers_.insert({key, value});
}

// ============================================================
// Http2Server 主逻辑
// ============================================================
Http2Server::Http2Server()  = default;
Http2Server::~Http2Server() { stop(); }

void Http2Server::Get(const std::string& p, Handler h) {
    routes_.push_back({"GET", std::regex(p), p, std::move(h)});
}
void Http2Server::Post(const std::string& p, Handler h) {
    routes_.push_back({"POST", std::regex(p), p, std::move(h)});
}
void Http2Server::Put(const std::string& p, Handler h) {
    routes_.push_back({"PUT", std::regex(p), p, std::move(h)});
}
void Http2Server::Delete(const std::string& p, Handler h) {
    routes_.push_back({"DELETE", std::regex(p), p, std::move(h)});
}
void Http2Server::Options(const std::string& p, Handler h) {
    routes_.push_back({"OPTIONS", std::regex(p), p, std::move(h)});
}

void Http2Server::set_pre_routing_handler(PreHandler h) {
    pre_handler_ = std::move(h);
}

bool Http2Server::listen(const std::string& addr, int port, size_t thread_pool_size) {
    running_ = true;

    // 初始化线程池
    thread_pool_ = std::make_unique<ThreadPool>(thread_pool_size);
    printf("[Http2Server] Thread pool: %zu workers\n",
           thread_pool_size > 0 ? thread_pool_size : std::thread::hardware_concurrency());

    // 创建 TCP socket
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        perror("[Http2Server] socket");
        return false;
    }

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(static_cast<uint16_t>(port));
    if (addr == "0.0.0.0")
        sa.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, addr.c_str(), &sa.sin_addr);

    if (::bind(listen_fd_, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("[Http2Server] bind");
        ::close(listen_fd_);
        return false;
    }
    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        perror("[Http2Server] listen");
        ::close(listen_fd_);
        return false;
    }

    printf("[Http2Server] HTTP/2 (h2c) listening on %s:%d\n",
           addr.c_str(), port);

    // Accept 循环
    while (running_.load(std::memory_order_acquire)) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd_, (struct sockaddr*)&client_addr,
                                  &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (!running_) break;
            perror("[Http2Server] accept");
            continue;
        }

        // 每连接一个线程
        std::thread(handle_connection, client_fd, this).detach();
    }

    ::close(listen_fd_);
    listen_fd_ = -1;
    return true;
}

void Http2Server::stop() {
    if (!running_.exchange(false)) return;
    // 关闭监听 socket 让 accept() 退出
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}
