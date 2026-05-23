add_library(piggy_memory STATIC
    src/segment_arena.cpp
    src/term_hash_table.cpp
)

target_include_directories(piggy_memory PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(piggy_memory PUBLIC piggy_core)
