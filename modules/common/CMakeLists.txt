add_library(piggy_common STATIC
    src/thread_pool.cpp
    src/file_utils.cpp
)

target_include_directories(piggy_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(piggy_common PUBLIC piggy_core)
