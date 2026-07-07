cmake_minimum_required(VERSION 3.20)
project(trace_graph LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(MARKOV_TRACE_GRAPH_DEBUG_DEFAULT OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(MARKOV_TRACE_GRAPH_DEBUG_DEFAULT ON)
endif()

option(TRACE_GRAPH_DEBUG "Build trace_graph diagnostics and validation code" ${MARKOV_TRACE_GRAPH_DEBUG_DEFAULT})

add_library(trace_graph_options INTERFACE)
target_compile_features(trace_graph_options INTERFACE cxx_std_23)
target_include_directories(trace_graph_options
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../third_party/ktransformers/archive/third_party
)
if(TRACE_GRAPH_DEBUG)
    target_compile_definitions(trace_graph_options INTERFACE DEBUG)
endif()

add_library(trace_graph_core STATIC)
target_sources(trace_graph_core
    PRIVATE
        src/core/trace_event.cpp
        src/core/dag_graph.cpp
        src/core/dag_builder.cpp
        src/core/logger.cpp
)
target_link_libraries(trace_graph_core PUBLIC trace_graph_options)

add_library(trace_graph_io STATIC)
target_sources(trace_graph_io
    PRIVATE
        src/io/chrome_trace_io.cpp
        src/io/trace_manifest_input.cpp
)
target_link_libraries(trace_graph_io PUBLIC trace_graph_core)

add_library(trace_graph_frontend STATIC)
target_sources(trace_graph_frontend
    PRIVATE
        src/frontend/model_config.cpp
        src/frontend/trace_normalizer.cpp
)
target_link_libraries(trace_graph_frontend PUBLIC trace_graph_core)

add_library(trace_graph_simulation STATIC)
target_sources(trace_graph_simulation
    PRIVATE
        src/simulation/topological_simulator.cpp
)
target_link_libraries(trace_graph_simulation PUBLIC trace_graph_core)

add_library(trace_graph_hicache STATIC)
target_sources(trace_graph_hicache
    PRIVATE
        src/modules/hicache/fact.cpp
        src/modules/hicache/router.cpp
        src/modules/hicache/policy.cpp
        src/modules/hicache/radix/token_radix_tree.cpp
        src/modules/hicache/radix/node_split_policy.cpp
        src/modules/hicache/runtime/token_store.cpp
        src/modules/hicache/runtime/target_pager.cpp
        src/modules/hicache/runtime/capacity_index.cpp
        src/modules/hicache/runtime/ref_ledger.cpp
        src/modules/hicache/runtime/target_control_clock.cpp
        src/modules/hicache/runtime/async_state.cpp
        src/modules/hicache/runtime/state_index.cpp
        src/modules/hicache/storage/storage_directory.cpp
        src/modules/hicache/model/state.cpp
        src/modules/hicache/model/request_model.cpp
        src/modules/hicache/model/prefetch_model.cpp
        src/modules/hicache/model/host_storage_model.cpp
        src/modules/hicache/model/writeback_model.cpp
        src/modules/hicache/model/finalizer.cpp
        src/modules/hicache/model/apply_model.cpp
)
target_link_libraries(trace_graph_hicache PUBLIC trace_graph_core trace_graph_frontend crypto)

add_library(trace_graph_modules STATIC)
target_sources(trace_graph_modules
    PRIVATE
        src/modules/node_scale/node_scale_module.cpp
        src/modules/hicache/hicache_module.cpp
)
target_link_libraries(trace_graph_modules
    PUBLIC
        trace_graph_core
        trace_graph_frontend
        trace_graph_hicache
        trace_graph_simulation
)

add_library(trace_graph_cli_support STATIC)
if(TRACE_GRAPH_DEBUG)
    add_library(trace_graph_diagnostics STATIC)
    target_sources(trace_graph_diagnostics
        PRIVATE
            src/modules/dag_analysis/diagnostics.cpp
            src/modules/diagnostics/json_summary_writer.cpp
            src/modules/hicache/diagnostics/summary.cpp
    )
    target_link_libraries(trace_graph_diagnostics PUBLIC trace_graph_modules)

    add_library(trace_graph_validation STATIC)
    target_sources(trace_graph_validation
        PRIVATE
            src/modules/validation/validation_runner.cpp
    )
    target_link_libraries(trace_graph_validation PUBLIC trace_graph_modules)

    target_sources(trace_graph_cli_support PRIVATE src/cli/debug_support_debug.cpp)
    target_link_libraries(trace_graph_cli_support PUBLIC trace_graph_diagnostics trace_graph_validation)
else()
    target_sources(trace_graph_cli_support PRIVATE src/cli/debug_support_release.cpp)
    target_link_libraries(trace_graph_cli_support PUBLIC trace_graph_modules)
endif()

add_executable(trace_graph)
target_sources(trace_graph
    PRIVATE
        src/cli/main.cpp
)
target_link_libraries(trace_graph
    PRIVATE
        trace_graph_core
        trace_graph_io
        trace_graph_frontend
        trace_graph_simulation
        trace_graph_modules
        trace_graph_cli_support
)

set_target_properties(trace_graph PROPERTIES
    OUTPUT_NAME trace_graph
)
