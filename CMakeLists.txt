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
add_subdirectory(modules/codec)
add_subdirectory(modules/analysis)
add_subdirectory(modules/field)
add_subdirectory(modules/store)
add_subdirectory(modules/index)
add_subdirectory(modules/query)

# Module headers come before include/ so "query/xxx.h" resolves to module headers
include_directories(modules/core/include)
include_directories(modules/codec/include)
include_directories(modules/analysis/include)
include_directories(modules/field/include)
include_directories(modules/store/include)
include_directories(modules/index/include)
include_directories(modules/query/include)
include_directories(include)

# ── library sources ──────────────────────────────────────────────
set(LIB_SOURCES
    src/schema/schema.cpp
    src/tokenizer/analyzer.cpp
    src/postings/pfor_delta.cpp
    src/postings/skiplist.cpp
    src/postings/posting_list.cpp
    src/postings/posting_iterator.cpp
    src/fastfield/fast_field_writer.cpp
    src/fastfield/fast_field_reader.cpp
    src/segment/segment_writer.cpp
    src/segment/segment_reader.cpp
    src/segment/segment_merger.cpp
    src/core/index_writer.cpp
    src/query/index_searcher.cpp
    src/query/term_scorer.cpp
    src/query/conjunction_scorer.cpp
    src/query/wand_scorer.cpp
    src/query/exclusion_scorer.cpp
    src/query/boolean_query.cpp
    src/query/query_parser.cpp
)

add_library(inverted_index_lib STATIC ${LIB_SOURCES})
target_include_directories(inverted_index_lib PUBLIC include)

# ── main demo ────────────────────────────────────────────────────
add_executable(demo src/core/main.cpp)
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

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
