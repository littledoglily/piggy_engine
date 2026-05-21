add_library(piggy_analysis STATIC
    src/analyzer.cpp
)
target_include_directories(piggy_analysis PUBLIC include)
target_link_libraries(piggy_analysis PUBLIC piggy_core)
