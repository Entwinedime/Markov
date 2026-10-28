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

add_library(markov_source
    src/markov/trace_graph/source/manifest.cpp
    src/markov/trace_graph/source/chrome_reader.cpp
    src/markov/trace_graph/source/observations.cpp
    src/markov/trace_graph/source/clients.cpp
    src/markov/trace_graph/source/build.cpp
    src/markov/trace_graph/source/context.cpp
    src/markov/trace_graph/source/services.cpp
    src/markov/trace_graph/planning/token_paths.cpp
    src/markov/trace_graph/planning/radix_tree.cpp
    src/markov/trace_graph/planning/cache.cpp
    src/markov/trace_graph/planning/compute.cpp
    src/markov/trace_graph/planning/compute_plan.cpp
)
target_link_libraries(markov_source PUBLIC markov_graph nlohmann_json::nlohmann_json)
target_compile_options(markov_source PRIVATE -Wall -Wextra -Wpedantic)

add_library(markov_cost src/markov/trace_graph/cost/models.cpp src/markov/trace_graph/cost/queries.cpp)
target_include_directories(markov_cost PUBLIC src)
target_compile_options(markov_cost PRIVATE -Wall -Wextra -Wpedantic)

add_library(markov_prediction
    src/markov/trace_graph/planning/compute_program.cpp
    src/markov/trace_graph/simulation/program_execution.cpp
)
target_link_libraries(markov_prediction PUBLIC markov_source markov_cost)
target_compile_options(markov_prediction PRIVATE -Wall -Wextra -Wpedantic)

add_library(markov_adapters
    src/markov/adapters/probes.cpp
    src/markov/adapters/costs.cpp
    src/markov/adapters/source.cpp
    src/markov/adapters/ascend/causality.cpp
    src/markov/adapters/ascend/trace.cpp
    src/markov/adapters/sglang/observations.cpp
)
target_link_libraries(markov_adapters PUBLIC markov_prediction)
if(TRACE_GRAPH_DEBUG)
    target_compile_definitions(markov_graph PUBLIC TRACE_GRAPH_DEBUG)
endif()

add_executable(markov_core src/markov/main.cpp src/markov/commands/source.cpp src/markov/commands/costs.cpp)
target_link_libraries(markov_core PRIVATE markov_adapters)
