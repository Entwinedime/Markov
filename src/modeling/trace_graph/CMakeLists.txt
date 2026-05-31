cmake_minimum_required(VERSION 3.16)
project(trace_graph LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(trace_graph
    src/core/activity_record.cpp
    src/core/logger.cpp
    src/core/trace_parser.cpp
    src/core/export_raw_trace.cpp
    src/core/trace_dag.cpp
    src/frontend/model_config.cpp
    src/frontend/trace_normalizer.cpp
    src/domains/cache_io/cache_model.cpp
    src/cli/main.cpp
)

target_include_directories(trace_graph
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(trace_graph PROPERTIES
    OUTPUT_NAME trace_graph
)
