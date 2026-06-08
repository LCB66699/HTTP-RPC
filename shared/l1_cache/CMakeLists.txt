# L1 进程内 LRU 缓存
add_library(l1_cache STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../server/src/l1_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../server/src/l1_invalidator.cpp
)
target_include_directories(l1_cache PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../server/include
)
target_link_libraries(l1_cache redis_client pthread)
