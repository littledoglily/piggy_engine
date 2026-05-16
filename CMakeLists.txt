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

include_directories(include)

# ── library sources ──────────────────────────────────────────────
set(LIB_SOURCES
    src/tokenizer/analyzer.cpp
    src/postings/pfor_delta.cpp
    src/postings/skiplist.cpp
    src/postings/posting_list.cpp
    src/fastfield/fast_field_writer.cpp
    src/fastfield/fast_field_reader.cpp
    src/segment/segment_writer.cpp
    src/segment/segment_reader.cpp
    src/segment/segment_merger.cpp
    src/core/index_writer.cpp
    src/query/index_searcher.cpp
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

add_executable(test_merger       tests/segment/test_merger.cpp)
target_link_libraries(test_merger inverted_index_lib)

add_executable(test_fast_field   tests/fastfield/test_fast_field.cpp)
target_link_libraries(test_fast_field inverted_index_lib)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
