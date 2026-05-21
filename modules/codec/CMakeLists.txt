add_library(piggy_codec STATIC
    src/pfor_delta.cpp
    src/skiplist.cpp
)
target_include_directories(piggy_codec PUBLIC include)
target_link_libraries(piggy_codec PUBLIC piggy_core)
