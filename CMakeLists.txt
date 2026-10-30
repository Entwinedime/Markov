cmake_minimum_required(VERSION 3.20)
project(markov LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TRACE_GRAPH_DEBUG "Enable detailed graph diagnostics" OFF)
find_package(nlohmann_json REQUIRED)

add_library(markov_graph
    src/markov/trace_graph/graph/graph.cpp
    src/markov/trace_graph/simulation/execution.cpp
)
target_include_directories(markov_graph PUBLIC src)
target_compile_options(markov_graph PRIVATE -Wall -Wextra -Wpedantic)

add_library(markov_work
    src/markov/trace_graph/planning/token_paths.cpp
    src/markov/trace_graph/planning/radix_tree.cpp
    src/markov/trace_graph/planning/cache.cpp
    src/markov/trace_graph/planning/compute.cpp
    src/markov/trace_graph/planning/native_calls.cpp
)
target_link_libraries(markov_work PUBLIC markov_graph)
target_compile_options(markov_work PRIVATE -Wall -Wextra -Wpedantic)

add_library(markov_source
    src/markov/trace_graph/source/manifest.cpp
    src/markov/trace_graph/source/chrome_reader.cpp
    src/markov/trace_graph/source/observations.cpp
    src/markov/trace_graph/source/clients.cpp
    src/markov/trace_graph/source/build.cpp
    src/markov/trace_graph/source/context.cpp
    src/markov/trace_graph/source/details.cpp
    src/markov/trace_graph/source/regions.cpp
    src/markov/trace_graph/source/native_work.cpp
    src/markov/trace_graph/source/native_queues.cpp
    src/markov/trace_graph/source/services.cpp
    src/markov/trace_graph/source/cpu_costs.cpp
    src/markov/trace_graph/planning/compute_plan.cpp
)
target_link_libraries(markov_source PUBLIC markov_work nlohmann_json::nlohmann_json)
target_compile_options(markov_source PRIVATE -Wall -Wextra -Wpedantic)

add_library(markov_cost
    src/markov/trace_graph/cost/models.cpp
    src/markov/trace_graph/cost/queries.cpp
    src/markov/trace_graph/cost/scaling.cpp
)
target_include_directories(markov_cost PUBLIC src)
target_link_libraries(markov_cost PUBLIC markov_source)
target_compile_options(markov_cost PRIVATE -Wall -Wextra -Wpedantic)

add_library(markov_prediction
    src/markov/trace_graph/planning/serial_program.cpp
    src/markov/trace_graph/planning/cache_program.cpp
    src/markov/trace_graph/planning/cache_controls.cpp
    src/markov/trace_graph/planning/storage.cpp
    src/markov/trace_graph/planning/compute_program.cpp
    src/markov/trace_graph/planning/region_compute.cpp
    src/markov/trace_graph/simulation/program_execution.cpp
)
target_link_libraries(markov_prediction PUBLIC markov_source markov_cost)
target_compile_options(markov_prediction PRIVATE -Wall -Wextra -Wpedantic)

add_library(markov_adapters
    src/markov/adapters/ktransformers/observations.cpp
    src/markov/adapters/ktransformers/prediction.cpp
    src/markov/adapters/ktransformers/work.cpp
    src/markov/adapters/probes.cpp
    src/markov/adapters/costs.cpp
    src/markov/adapters/source.cpp
    src/markov/adapters/prediction.cpp
    src/markov/adapters/replay.cpp
    src/markov/adapters/ascend/causality.cpp
    src/markov/adapters/ascend/trace.cpp
    src/markov/adapters/ascend/tensor_work.cpp
    src/markov/adapters/sglang/observations.cpp
    src/markov/adapters/sglang/region_observations.cpp
    src/markov/adapters/sglang/storage_observations.cpp
    src/markov/adapters/sglang/dense.cpp
    src/markov/adapters/sglang/dense_calls.cpp
    src/markov/adapters/sglang/cache_indices.cpp
    src/markov/adapters/sglang/cache.cpp
    src/markov/adapters/sglang/metadata.cpp
    src/markov/adapters/sglang/allocation.cpp
    src/markov/adapters/sglang/kernel_preparation.cpp
    src/markov/adapters/sglang/input_preparation.cpp
    src/markov/adapters/sglang/result_output.cpp
    src/markov/adapters/sglang/result_release.cpp
)
target_link_libraries(markov_adapters PUBLIC markov_prediction)
if(TRACE_GRAPH_DEBUG)
    target_compile_definitions(markov_graph PUBLIC TRACE_GRAPH_DEBUG)
endif()

add_library(markov_reports
    src/markov/commands/io.cpp
    src/markov/commands/costs.cpp
    src/markov/commands/replay.cpp
    src/markov/commands/trajectory.cpp
)
target_link_libraries(markov_reports PUBLIC markov_adapters)

add_executable(markov_core
    src/markov/commands/serial_prediction.cpp
    src/markov/main.cpp
    src/markov/commands/source.cpp
    src/markov/commands/page_work.cpp
    src/markov/commands/prediction.cpp
    src/markov/commands/cpu_costs.cpp
)
target_link_libraries(markov_core PRIVATE markov_reports)

add_executable(markov_evaluate
    src/markov/evaluation/main.cpp
    src/markov/evaluation/evaluation.cpp
)
target_link_libraries(markov_evaluate PRIVATE markov_reports)
