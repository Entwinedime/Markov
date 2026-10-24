cmake_minimum_required(VERSION 3.20)
project(markov LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json 3.11 REQUIRED)

add_library(markov_core
    src/markov/controller/serving.cpp
    src/markov/execution/protocol.cpp
    src/markov/execution/durations.cpp
    src/markov/execution/executor.cpp
    src/markov/contracts/expression.cpp
    src/markov/contracts/input_group.cpp
    src/markov/contracts/serving.cpp
    src/markov/contracts/topology.cpp
    src/markov/contracts/integer_data.cpp
    src/markov/graph/access.cpp
    src/markov/graph/region_graph.cpp
    src/markov/graph/source_graph.cpp
    src/markov/production/source_projection.cpp
    src/markov/production/execution_source.cpp
    src/markov/templates/library.cpp
    src/markov/templates/group_execution.cpp
    src/markov/templates/asset.cpp
    src/markov/templates/instance.cpp
    src/markov/templates/target_graph.cpp
    src/markov/templates/parallel_composition.cpp
)
target_include_directories(markov_core PUBLIC src)
target_link_libraries(markov_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(markov_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(markov src/markov/cli/main.cpp)
add_library(markov_application
    src/markov/adapters/frameworks/sglang/batches.cpp
    src/markov/adapters/frameworks/sglang/structure.cpp
    src/markov/adapters/frameworks/sglang/rank_structure.cpp
    src/markov/workflow/serving.cpp
)
target_link_libraries(markov_application PUBLIC markov_core)
target_compile_options(markov_application PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
target_link_libraries(markov PRIVATE markov_application)
target_compile_options(markov PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
