add_library(piggy_store STATIC
    src/posting_list.cpp
    src/posting_iterator.cpp
)
target_include_directories(piggy_store PUBLIC include)
target_link_libraries(piggy_store PUBLIC piggy_core piggy_codec)
