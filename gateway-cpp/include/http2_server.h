// ============================================================
// Http2Server — 基于 nghttp2 C API 的 HTTP/2 Server 封装
// 对外提供与 httplib::Server 兼容的同步式路由注册 + listen API
// 内部使用 nghttp2 C 库处理 HTTP/2 协议 (h2c cleartext)
// ============================================================
#pragma once

#include <string>
#include <functional>
#include <vector>
#include <regex>
#include <memory>
#include <map>
#include <thread>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include "thread_pool.h"

struct PendingResponse {
    int32_t stream_id = 0;
    int status = 200;
    std::string body;
    std::string content_type;
    std::multimap<std::string, std::string> headers;
};

struct ConnCtx {
    int fd = -1;
    int event_fd = -1;
    class Http2Server* server = nullptr;
    std::deque<PendingResponse> pending_responses;
    std::mutex response_mutex;
};

class Http2Server {
public:
    struct Request {
        std::string body;
        std::string method;
        std::string path;

        std::string get_header_value(const std::string& key) const;

        // URL query 参数（如 ?id=123）
        std::string get_param_value(const std::string& key) const;

        // regex 路由捕获组 (对应 httplib::Request::matches[1])
        std::vector<std::string> matches;

        // ---- multipart form ----
        struct FormFile {
            std::string filename;
            std::string content;
            std::string content_type;
        };

        class Form {
        public:
            FormFile get_file(const std::string& name) const;
            void parse(const std::string& content_type_header, const std::string& body);
        private:
            std::map<std::string, FormFile> files_;
        };
        Form form;

        // 内部使用
        std::multimap<std::string, std::string> headers;
        std::string query_string;
    };

    struct Response {
        int status = 200;

        void set_content(const std::string& body, const std::string& content_type);
        void set_header(const std::string& key, const std::string& value);

        std::string body_;
        std::string content_type_;
        std::multimap<std::string, std::string> headers_;
    };

    using Handler    = std::function<void(const Request&, Response&)>;

    enum class HandlerResponse { Handled, Unhandled };
    using PreHandler = std::function<HandlerResponse(const Request&, Response&)>;

    Http2Server();
    ~Http2Server();

    void Get(const std::string& pattern, Handler handler);
    void Post(const std::string& pattern, Handler handler);
    void Put(const std::string& pattern, Handler handler);
    void Delete(const std::string& pattern, Handler handler);
    void Options(const std::string& pattern, Handler handler);

    void set_pre_routing_handler(PreHandler handler);

    bool listen(const std::string& addr, int port, size_t thread_pool_size = 0);
    void stop();

private:
    struct Route {
        std::string method;
        std::regex  pattern;
        std::string pattern_str;
        Handler     handler;
    };

    static void handle_connection(int fd, Http2Server* self);
    static void submit_pending_responses(struct nghttp2_session* session, ConnCtx* conn);

    std::vector<Route> routes_;
    PreHandler         pre_handler_;
    std::atomic<bool>  running_{false};
    int                listen_fd_ = -1;
    std::unique_ptr<ThreadPool> thread_pool_;

public:
    // 供 nghttp2 回调 on_frame_recv_callback 调用（非成员静态函数需 public）
    static void process_request_async(Http2Server* server, ConnCtx* conn, int32_t stream_id,
                                      std::string method, std::string path, std::string query,
                                      std::multimap<std::string, std::string> headers,
                                      std::string body);
private:
};
