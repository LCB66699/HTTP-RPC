cmake_minimum_required(VERSION 3.16)
project(http-rpc VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -I${CMAKE_SOURCE_DIR} -I${CMAKE_SOURCE_DIR}/server -I${CMAKE_SOURCE_DIR}/server/include -I${CMAKE_SOURCE_DIR}/server/generated")

# Dependencies
find_package(OpenSSL REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED mysqlclient)
pkg_check_modules(HIREDIS REQUIRED hiredis)
pkg_check_modules(RABBITMQ REQUIRED librabbitmq)
find_package(GTest REQUIRED)

# Redis++ — build as subproject
set(REDIS_PLUS_PLUS_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(REDIS_PLUS_PLUS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(REDIS_PLUS_PLUS_CXX_STANDARD 20 CACHE STRING "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/vendor/redis-plus-plus ${CMAKE_BINARY_DIR}/redis++)
include_directories(${CMAKE_SOURCE_DIR}/vendor/redis-plus-plus/src)
target_link_libraries(redis++ PUBLIC ${HIREDIS_LIBRARIES})

# OpenTelemetry C++ — auto-detect, skip if headers not installed
set(OTEL_LIBS "")
set(HAS_OTEL OFF)
if(EXISTS "/usr/local/include/opentelemetry/exporters/otlp/otlp_http_exporter.h")
    message(STATUS "OTel HTTP exporter found")
    set(HAS_OTEL ON)
    set(OTEL_LIBS
        -Wl,--start-group
        opentelemetry_exporter_otlp_http opentelemetry_exporter_otlp_http_client
        opentelemetry_otlp_recordable opentelemetry_logs opentelemetry_proto
        opentelemetry_trace opentelemetry_resources opentelemetry_common
        opentelemetry_http_client_curl
        -Wl,--end-group
        curl)
elseif(EXISTS "/usr/local/include/opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h")
    message(STATUS "OTel gRPC exporter found")
    set(HAS_OTEL ON)
    set(OTEL_LIBS
        -Wl,--start-group
        opentelemetry_exporter_otlp_grpc opentelemetry_exporter_otlp_grpc_client
        opentelemetry_otlp_recordable opentelemetry_logs
        opentelemetry_trace opentelemetry_resources opentelemetry_version
        opentelemetry_proto opentelemetry_proto_grpc
        opentelemetry_common
        -Wl,--end-group)
else()
    message(STATUS "OTel NOT found — tracing disabled")
endif()
add_compile_definitions(HAS_OTEL=$<BOOL:${HAS_OTEL}>)

# Proto generation
set(PROTO_DIR ${CMAKE_SOURCE_DIR}/proto)
set(GEN_DIR ${CMAKE_SOURCE_DIR}/server/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

set(PROTO_FILES
    ${PROTO_DIR}/rpc_auth.proto
    ${PROTO_DIR}/rpc_file.proto
    ${PROTO_DIR}/rpc_spreadsheet.proto
    ${PROTO_DIR}/rpc_health.proto
    ${PROTO_DIR}/rpc_search.proto
    ${PROTO_DIR}/rpc_sharing.proto)

foreach(PROTO ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO} NAME_WLE)
    list(APPEND PROTO_SRCS ${GEN_DIR}/${PROTO_NAME}.pb.cc)
    list(APPEND GRPC_SRCS ${GEN_DIR}/${PROTO_NAME}.grpc.pb.cc)
endforeach()

add_custom_command(
    OUTPUT ${PROTO_SRCS} ${GRPC_SRCS}
    COMMAND protoc -I ${PROTO_DIR} --cpp_out=${GEN_DIR} --grpc_out=${GEN_DIR} --plugin=protoc-gen-grpc=/usr/bin/grpc_cpp_plugin ${PROTO_FILES}
    DEPENDS ${PROTO_FILES}
    COMMENT "Generating protobuf/gRPC code"
)
set(PROTO_LIB_SRCS ${PROTO_SRCS} ${GRPC_SRCS})

# Shared sources between services
set(SHARED_SRCS
    server/src/auth_interceptor.cpp
    server/src/call_logger.cpp
    server/src/database.cpp
    server/src/redis_client.cpp)
if(HAS_OTEL)
    list(APPEND SHARED_SRCS server/src/otel_tracer.cpp)
endif()

# ---- Services ----
function(add_rpc_service NAME MAIN_SRC EXTRA_SRCS)
    add_executable(rpc_${NAME} ${MAIN_SRC} ${SHARED_SRCS} ${EXTRA_SRCS} ${PROTO_LIB_SRCS})
    target_link_libraries(rpc_${NAME}
        gRPC::grpc++ gRPC::grpc++_reflection ${Protobuf_LIBRARIES}
        OpenSSL::SSL OpenSSL::Crypto
        ${MYSQL_LIBRARIES} ${HIREDIS_LIBRARIES} redis++_static pthread nghttp2
        ${RABBITMQ_LIBRARIES}
        ${OTEL_LIBS})
    if(HAS_OTEL)
        target_compile_definitions(rpc_${NAME} PRIVATE HAS_OTEL=1)
    endif()
endfunction()

add_rpc_service(auth   server/src/main_auth.cpp     "server/src/auth_service_impl.cpp;server/src/health_service_impl.cpp")
add_rpc_service(sheet  server/src/main_sheet.cpp    "server/src/spreadsheet_service_impl.cpp;server/src/health_service_impl.cpp;server/src/l1_cache.cpp;server/src/l1_invalidator.cpp;server/src/rabbit_publisher.cpp")
add_rpc_service(file   server/src/main_file.cpp     "server/src/file_service_impl.cpp;server/src/health_service_impl.cpp;server/src/l1_cache.cpp;server/src/rabbit_publisher.cpp")
add_rpc_service(search server/src/main_search.cpp   "server/src/search_service_impl.cpp;server/src/health_service_impl.cpp")

# ---- Tests ----
add_executable(cpp_test
    test/test_sha256.cpp test/test_jwt.cpp test/test_snowflake.cpp
    test/test_otel_helpers.cpp
    test/test_sheet_helpers.cpp test/test_file_helpers.cpp
    test/test_data_helpers.cpp)
target_link_libraries(cpp_test GTest::gtest GTest::gtest_main OpenSSL::SSL OpenSSL::Crypto pthread)

include(GoogleTest)
gtest_discover_tests(cpp_test)
