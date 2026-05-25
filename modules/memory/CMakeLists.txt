add_library(piggy_memory STATIC
    src/segment_arena.cpp
    src/term_hash_table.cpp
    src/memory_segment.cpp
    src/memory_posting_iterator.cpp
    src/memory_segment_reader.cpp
    src/flush_queue.cpp
    src/flush_worker.cpp
    src/rt_index_thread.cpp
    src/realtime_index_manager.cpp
)

target_include_directories(piggy_memory PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(piggy_memory PUBLIC
    piggy_core
    piggy_analysis
    piggy_field
    piggy_store
    piggy_index
    piggy_query
)
