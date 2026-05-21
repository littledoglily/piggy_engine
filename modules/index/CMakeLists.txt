add_library(piggy_index STATIC
    src/segment_writer.cpp
    src/segment_reader.cpp
    src/segment_merger.cpp
    src/index_writer.cpp
)
target_include_directories(piggy_index PUBLIC include)
target_link_libraries(piggy_index PUBLIC piggy_core piggy_codec piggy_store piggy_field piggy_analysis)
