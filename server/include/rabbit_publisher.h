#pragma once
#include <string>
#include <memory>
#include <amqp.h>
#include <amqp_tcp_socket.h>

// 轻量 RabbitMQ 发布器 — 连接池复用, 线程安全
class RabbitPublisher {
public:
    RabbitPublisher(const std::string& host, int port,
                    const std::string& user, const std::string& pass);
    ~RabbitPublisher();

    bool Publish(const std::string& exchange, const std::string& routing_key,
                 const std::string& body);

private:
    bool Connect();
    std::string host_;
    int port_;
    std::string user_, pass_;
    amqp_connection_state_t conn_ = nullptr;
    amqp_socket_t* socket_ = nullptr;
};
