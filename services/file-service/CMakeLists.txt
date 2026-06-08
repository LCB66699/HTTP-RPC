# ============================================================
# File Service — 文件上传/下载微服务
# 数据库: rpc_file_{0,1} MySQL (独占) + MinIO + Redis 缓存
# ============================================================
cmake_minimum_required(VERSION 3.16)
project(file-service VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../../shared/redis_client ${CMAKE_BINARY_DIR}/shared/redis)
add_subdirectory(../../shared/snowflake    ${CMAKE_BINARY_DIR}/shared/snowflake)
add_subdirectory(../../shared/l1_cache     ${CMAKE_BINARY_DIR}/shared/l1_cache)
add_subdirectory(../../shared/circuit_breaker ${CMAKE_BINARY_DIR}/shared/cb)

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(CURL REQUIRED)

set(PROTO_DIR ${CMAKE_SOURCE_DIR}/../../proto)
protobuf_generate_cpp(FILE_PROTO_SRCS FILE_PROTO_HDRS
    ${PROTO_DIR}/rpc_file.proto ${PROTO_DIR}/rpc_auth.proto)
grpc_generate_cpp(FILE_GRPC_SRCS FILE_GRPC_HDRS
    ${PROTO_DIR}/rpc_file.proto ${PROTO_DIR}/rpc_auth.proto)

add_executable(file-service
    src/main.cpp
    src/file_impl.cpp
    src/minio_client.cpp
    src/event_publisher.cpp
    ${FILE_PROTO_SRCS} ${FILE_GRPC_SRCS}
)

target_include_directories(file-service PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${PROTO_DIR} ${CMAKE_BINARY_DIR}
)

target_link_libraries(file-service
    redis_client snowflake l1_cache circuit_breaker
    protobuf::libprotobuf gRPC::grpc++
    libmysqlclient pthread ssl crypto CURL::libcurl
    # RabbitMQ client (SimpleAmqpClient 或 AMQP-CPP)
)
