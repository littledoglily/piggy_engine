cmake_minimum_required(VERSION 3.16)
project(InvertedIndex VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_DEBUG   "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

add_subdirectory(modules/core)
add_subdirectory(modules/common)
add_subdirectory(modules/codec)
add_subdirectory(modules/analysis)
add_subdirectory(modules/field)
add_subdirectory(modules/store)
add_subdirectory(modules/index)
add_subdirectory(modules/query)

# Umbrella interface library: links all modules
add_library(inverted_index_lib INTERFACE)
target_link_libraries(inverted_index_lib INTERFACE
    piggy_query
    piggy_index
    piggy_field
    piggy_store
    piggy_analysis
    piggy_codec
    piggy_core
)

# ── main demo ────────────────────────────────────────────────────
add_executable(demo tools/demo.cpp)
target_link_libraries(demo inverted_index_lib)

# ── unit tests ───────────────────────────────────────────────────
add_executable(test_all tests/test_all.cpp)
target_link_libraries(test_all inverted_index_lib)

add_executable(test_analyzer     tests/tokenizer/test_analyzer.cpp)
target_link_libraries(test_analyzer inverted_index_lib)

add_executable(test_pfor_delta   tests/postings/test_pfor_delta.cpp)
target_link_libraries(test_pfor_delta inverted_index_lib)

add_executable(test_skiplist     tests/postings/test_skiplist.cpp)
target_link_libraries(test_skiplist inverted_index_lib)

add_executable(test_posting_list tests/postings/test_posting_list.cpp)
target_link_libraries(test_posting_list inverted_index_lib)

add_executable(test_posting_iterator tests/postings/test_posting_iterator.cpp)
target_link_libraries(test_posting_iterator inverted_index_lib)

add_executable(test_merger          tests/segment/test_merger.cpp)
target_link_libraries(test_merger inverted_index_lib)

add_executable(test_per_field_index tests/segment/test_per_field_index.cpp)
target_link_libraries(test_per_field_index inverted_index_lib)

add_executable(test_fast_field   tests/fastfield/test_fast_field.cpp)
target_link_libraries(test_fast_field inverted_index_lib)

add_executable(test_schema       tests/schema/test_schema.cpp)
target_link_libraries(test_schema inverted_index_lib)

add_executable(wiki_indexer      tools/wiki_indexer.cpp)
target_link_libraries(wiki_indexer inverted_index_lib)

add_executable(wiki_searcher     tools/wiki_searcher.cpp)
target_link_libraries(wiki_searcher inverted_index_lib)

add_executable(bench_search      tools/bench_search.cpp)
target_link_libraries(bench_search inverted_index_lib)

add_executable(tim_reader        tools/tim_reader.cpp)
# tim_reader 纯标准库，无需 inverted_index_lib

add_executable(doc_reader        tools/doc_reader.cpp)
target_link_libraries(doc_reader inverted_index_lib)

add_executable(test_term_scorer  tests/query/test_term_scorer.cpp)
target_link_libraries(test_term_scorer inverted_index_lib)

add_executable(test_conjunction_scorer tests/query/test_conjunction_scorer.cpp)
target_link_libraries(test_conjunction_scorer inverted_index_lib)

add_executable(test_wand_scorer      tests/query/test_wand_scorer.cpp)
target_link_libraries(test_wand_scorer inverted_index_lib)

add_executable(test_exclusion_scorer tests/query/test_exclusion_scorer.cpp)
target_link_libraries(test_exclusion_scorer inverted_index_lib)

add_executable(test_query            tests/query/test_query.cpp)
target_link_libraries(test_query     inverted_index_lib)

add_executable(test_query_parser     tests/query/test_query_parser.cpp)
target_link_libraries(test_query_parser inverted_index_lib)

add_executable(test_index_searcher   tests/query/test_index_searcher.cpp)
target_link_libraries(test_index_searcher inverted_index_lib)

add_executable(test_block_max_wand   tests/query/test_block_max_wand.cpp)
target_link_libraries(test_block_max_wand inverted_index_lib)

add_executable(test_scorer_tree_e2e  tests/query/test_scorer_tree_e2e.cpp)
target_link_libraries(test_scorer_tree_e2e inverted_index_lib)

# ── parallel 索引构建测试 ────────────────────────────────────────────────────
add_executable(test_parallel_worker_equivalence
    tests/parallel/test_parallel_worker_equivalence.cpp)
target_link_libraries(test_parallel_worker_equivalence inverted_index_lib)

# ── common 模块单元测试 ──────────────────────────────────────────────────────
add_executable(test_blocking_queue   tests/common/test_blocking_queue.cpp)
target_link_libraries(test_blocking_queue piggy_common)

add_executable(test_thread_pool      tests/common/test_thread_pool.cpp)
target_link_libraries(test_thread_pool piggy_common)

add_executable(test_kway_merge       tests/common/test_kway_merge.cpp)
target_link_libraries(test_kway_merge piggy_common)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
