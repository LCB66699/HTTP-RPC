# 断路器共享库 (多维熔断 + 副本隔离)
add_library(circuit_breaker INTERFACE)
target_include_directories(circuit_breaker INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../gateway-cpp/include
)
