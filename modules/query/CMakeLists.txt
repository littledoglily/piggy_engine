add_library(piggy_query STATIC
    src/index_searcher.cpp
    src/term_scorer.cpp
    src/conjunction_scorer.cpp
    src/wand_scorer.cpp
    src/exclusion_scorer.cpp
    src/boolean_query.cpp
    src/query_parser.cpp
)
target_include_directories(piggy_query PUBLIC include)
target_link_libraries(piggy_query PUBLIC piggy_core piggy_store piggy_index piggy_analysis)
