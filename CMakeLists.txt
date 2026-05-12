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
    src/analyzer.cpp
    src/pfor_delta.cpp
    src/skiplist.cpp
    src/posting_list.cpp
    src/segment_writer.cpp
    src/segment_reader.cpp
    src/segment_merger.cpp
    src/index_writer.cpp
    src/index_searcher.cpp
)

add_library(inverted_index_lib STATIC ${LIB_SOURCES})
target_include_directories(inverted_index_lib PUBLIC include)

# ── main demo ────────────────────────────────────────────────────
add_executable(demo src/main.cpp)
target_link_libraries(demo inverted_index_lib)

# ── unit tests ───────────────────────────────────────────────────
add_executable(test_all tests/test_all.cpp)
target_link_libraries(test_all inverted_index_lib)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
