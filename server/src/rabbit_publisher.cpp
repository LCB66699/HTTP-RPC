#include "rabbit_publisher.h"
#include <cstdio>
#include <nlohmann/json.hpp>

RabbitPublisher::RabbitPublisher(const std::string &host, int port,
                                 const std::string &user,
                                 const std::string &pass)
    : host_(host), port_(port), user_(user), pass_(pass) {}

RabbitPublisher::~RabbitPublisher() {
  if (conn_) {
    amqp_channel_close(conn_, 1, AMQP_REPLY_SUCCESS);
    amqp_connection_close(conn_, AMQP_REPLY_SUCCESS);
    amqp_destroy_connection(conn_);
  }
}

bool RabbitPublisher::Connect() {
  if (conn_)
    return true;
  conn_ = amqp_new_connection();
  socket_ = amqp_tcp_socket_new(conn_);
  if (!socket_) {
    fprintf(stderr, "[RabbitMQ] socket create failed\n");
    return false;
  }
  int st = amqp_socket_open(socket_, host_.c_str(), port_);
  if (st) {
    fprintf(stderr, "[RabbitMQ] connect failed\n");
    return false;
  }
  amqp_rpc_reply_t reply =
      amqp_login(conn_, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN,
                 user_.c_str(), pass_.c_str());
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    fprintf(stderr, "[RabbitMQ] login failed\n");
    return false;
  }
  amqp_channel_open(conn_, 1);
  reply = amqp_get_rpc_reply(conn_);
  if (reply.reply_type != AMQP_RESPONSE_NORMAL) {
    fprintf(stderr, "[RabbitMQ] channel open failed\n");
    return false;
  }
  return true;
}

bool RabbitPublisher::Publish(const std::string &exchange,
                              const std::string &routing_key,
                              const std::string &body) {
  if (!Connect())
    return false;
  amqp_bytes_t msg = amqp_cstring_bytes(body.c_str());
  int st = amqp_basic_publish(conn_, 1, amqp_cstring_bytes(exchange.c_str()),
                              amqp_cstring_bytes(routing_key.c_str()), 0, 0,
                              nullptr, msg);
  if (st < 0) {
    fprintf(stderr, "[RabbitMQ] publish failed: %d\n", st);
    return false;
  }
  return true;
}
