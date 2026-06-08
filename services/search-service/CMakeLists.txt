# ============================================================
# Search Service — 全文检索微服务
# 数据库: Elasticsearch + MongoDB (独占)
# ============================================================
cmake_minimum_required(VERSION 3.16)
project(search-service VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(PROTO_DIR ${CMAKE_SOURCE_DIR}/../../proto)

# Search 直接调 ES REST API，不需要复杂依赖
add_executable(search-service
    src/main.cpp
    src/search_impl.cpp
    src/es_client.cpp
)

target_include_directories(search-service PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${PROTO_DIR}
    ${CMAKE_BINARY_DIR}
)

target_link_libraries(search-service
    protobuf::libprotobuf gRPC::grpc++
    libcurl pthread ssl crypto
)
