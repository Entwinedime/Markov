cmake_minimum_required(VERSION 3.20)
project(trace_graph LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(trace_graph
    src/core/trace_event.cpp
    src/core/dag_graph.cpp
    src/core/dag_builder.cpp
    src/core/logger.cpp
    src/io/chrome_trace_io.cpp
    src/simulation/topological_simulator.cpp
    src/frontend/model_config.cpp
    src/frontend/trace_normalizer.cpp
    src/modules/node_scale_module.cpp
    src/modules/hicache/hicache_fact.cpp
    src/modules/hicache/hicache_router.cpp
    src/modules/hicache/hicache_token_store.cpp
    src/modules/hicache/hicache_target_pager.cpp
    src/modules/hicache/hicache_token_radix_tree.cpp
    src/modules/hicache/hicache_state_index.cpp
    src/modules/hicache/hicache_model.cpp
    src/modules/hicache/hicache_summary.cpp
    src/modules/hicache/hicache_module.cpp
    src/cli/main.cpp
)

target_include_directories(trace_graph
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/ktransformers/archive/third_party
)

target_link_libraries(trace_graph PRIVATE crypto)

set_target_properties(trace_graph PROPERTIES
    OUTPUT_NAME trace_graph
)
