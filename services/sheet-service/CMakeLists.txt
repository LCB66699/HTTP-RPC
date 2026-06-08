# ============================================================
# Sheet Service — 表格 CRUD 微服务
# 数据库: rpc_spreadsheet_{0,1} MySQL (独占) + Redis 缓存
# ============================================================
cmake_minimum_required(VERSION 3.16)
project(sheet-service VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../../shared/redis_client ${CMAKE_BINARY_DIR}/shared/redis)
add_subdirectory(../../shared/snowflake    ${CMAKE_BINARY_DIR}/shared/snowflake)
add_subdirectory(../../shared/l1_cache     ${CMAKE_BINARY_DIR}/shared/l1_cache)
add_subdirectory(../../shared/circuit_breaker ${CMAKE_BINARY_DIR}/shared/cb)

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(PROTO_DIR ${CMAKE_SOURCE_DIR}/../../proto)
protobuf_generate_cpp(SHEET_PROTO_SRCS SHEET_PROTO_HDRS
    ${PROTO_DIR}/rpc_spreadsheet.proto ${PROTO_DIR}/rpc_auth.proto)
grpc_generate_cpp(SHEET_GRPC_SRCS SHEET_GRPC_HDRS
    ${PROTO_DIR}/rpc_spreadsheet.proto ${PROTO_DIR}/rpc_auth.proto)

add_executable(sheet-service
    src/main.cpp
    src/sheet_impl.cpp
    ${SHEET_PROTO_SRCS} ${SHEET_GRPC_SRCS}
)

target_include_directories(sheet-service PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${PROTO_DIR} ${CMAKE_BINARY_DIR}
)

target_link_libraries(sheet-service
    redis_client snowflake l1_cache circuit_breaker
    protobuf::libprotobuf gRPC::grpc++
    libmysqlclient pthread ssl crypto
)
