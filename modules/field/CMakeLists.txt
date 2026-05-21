add_library(piggy_field STATIC
    src/schema.cpp
    src/fast_field_writer.cpp
    src/fast_field_reader.cpp
)
target_include_directories(piggy_field PUBLIC include)
target_link_libraries(piggy_field PUBLIC piggy_core piggy_analysis)
