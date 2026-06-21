#include "shared/client/rabbit_publisher.h"

#include <cstdio>
#include <nlohmann/json.hpp>

RabbitPublisher::RabbitPublisher(const std::string &host, int port, const std::string &user, const std::string &pass)
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
    amqp_rpc_reply_t reply = amqp_login(conn_, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, user_.c_str(), pass_.c_str());
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

bool RabbitPublisher::Publish(const std::string &exchange, const std::string &routing_key, const std::string &body) {
    if (!Connect())
        return false;
    amqp_bytes_t msg = amqp_cstring_bytes(body.c_str());
    int st = amqp_basic_publish(conn_, 1, amqp_cstring_bytes(exchange.c_str()), amqp_cstring_bytes(routing_key.c_str()),
                                0, 0, nullptr, msg);
    if (st < 0) {
        fprintf(stderr, "[RabbitMQ] publish failed: %d\n", st);
        return false;
    }
    return true;
}

bool RabbitPublisher::PublishWithTrace(const std::string &exchange, const std::string &routing_key,
                                       const std::string &body, const std::string &traceparent,
                                       const std::string &tracestate) {
    if (!Connect())
        return false;

    amqp_basic_properties_t props;
    memset(&props, 0, sizeof(props));
    props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_HEADERS_FLAG;
    props.content_type = amqp_cstring_bytes("application/json");

    int n = 1 + (tracestate.empty() ? 0 : 1);
    amqp_table_entry_t *entries = new amqp_table_entry_t[n];

    entries[0].key = amqp_cstring_bytes("traceparent");
    entries[0].value.kind = AMQP_FIELD_KIND_UTF8;
    entries[0].value.value.bytes = amqp_cstring_bytes(traceparent.c_str());

    if (!tracestate.empty()) {
        entries[1].key = amqp_cstring_bytes("tracestate");
        entries[1].value.kind = AMQP_FIELD_KIND_UTF8;
        entries[1].value.value.bytes = amqp_cstring_bytes(tracestate.c_str());
    }

    props.headers.num_entries = n;
    props.headers.entries = entries;

    amqp_bytes_t msg = amqp_cstring_bytes(body.c_str());
    int st = amqp_basic_publish(conn_, 1, amqp_cstring_bytes(exchange.c_str()),
                                amqp_cstring_bytes(routing_key.c_str()), 0, 0, &props, msg);
    delete[] entries;

    if (st < 0) {
        fprintf(stderr, "[RabbitMQ] publish failed: %d\n", st);
        return false;
    }
    return true;
}
