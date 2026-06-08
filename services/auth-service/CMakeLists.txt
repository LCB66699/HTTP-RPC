# Auth Service — 认证与用户管理微服务
cmake_minimum_required(VERSION 3.16)
project(auth-service VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared libs
add_subdirectory(../../shared/jwt      ${CMAKE_BINARY_DIR}/shared/jwt)
add_subdirectory(../../shared/redis_client ${CMAKE_BINARY_DIR}/shared/redis)
add_subdirectory(../../shared/snowflake    ${CMAKE_BINARY_DIR}/shared/snowflake)

# Proto generation — must match existing include paths (generated/ subdir)
set(PROTO_DIR ${CMAKE_SOURCE_DIR}/../../proto)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)
set(PB_SRC ${CMAKE_BINARY_DIR}/generated/rpc_auth.pb.cc)
set(GRPC_SRC ${CMAKE_BINARY_DIR}/generated/rpc_auth.grpc.pb.cc)

add_custom_command(
    OUTPUT ${PB_SRC} ${GRPC_SRC}
    COMMAND protoc -I${PROTO_DIR} --cpp_out=${CMAKE_BINARY_DIR}/generated ${PROTO_DIR}/rpc_auth.proto
    COMMAND protoc -I${PROTO_DIR} --grpc_out=${CMAKE_BINARY_DIR}/generated --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin ${PROTO_DIR}/rpc_auth.proto
    DEPENDS ${PROTO_DIR}/rpc_auth.proto
)

# Executable — 引用 server/ 实际源码，不重复
file(GLOB AUTH_SRCS
    ${CMAKE_SOURCE_DIR}/../../server/src/auth_service_impl.cpp
    ${CMAKE_SOURCE_DIR}/../../server/src/auth_interceptor.cpp
    ${CMAKE_SOURCE_DIR}/../../server/src/database.cpp
    ${CMAKE_SOURCE_DIR}/../../server/src/redis_client.cpp
    ${CMAKE_SOURCE_DIR}/../../server/src/call_logger.cpp
)
add_executable(auth-service
    src/main.cpp
    ${AUTH_SRCS}
    ${PB_SRC} ${GRPC_SRC}
)

target_include_directories(auth-service PRIVATE
    ${CMAKE_SOURCE_DIR}/../../server/include
    ${CMAKE_SOURCE_DIR}/../../gateway-cpp/include
    ${PROTO_DIR}
    ${CMAKE_BINARY_DIR}
)

find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

target_link_libraries(auth-service
    jwt redis_client snowflake
    protobuf::libprotobuf gRPC::grpc++ gRPC::grpc++_reflection
    libmysqlclient pthread ssl crypto
)
