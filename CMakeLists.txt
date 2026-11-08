cmake_minimum_required(VERSION 3.20)
project(markov LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json 3.11 REQUIRED)

add_library(markov_core
    src/markov/controller/native.cpp
    src/markov/controller/delivery.cpp
    src/markov/resources/kv_pool.cpp
    src/markov/resources/service.cpp
    src/markov/resources/slot_pool.cpp
    src/markov/resources/prefix_index.cpp
    src/markov/contracts/expression.cpp
    src/markov/contracts/serving.cpp
    src/markov/graph/types.cpp
    src/markov/graph/state.cpp
    src/markov/graph/observation.cpp
    src/markov/graph/graph.cpp
    src/markov/execution/timeline.cpp
    src/markov/execution/dependencies.cpp
    src/markov/execution/communication.cpp
    src/markov/serving/session.cpp
    src/markov/controller/boundary_times.cpp
    src/markov/production/projection.cpp
    src/markov/production/templates.cpp
    src/markov/production/entries.cpp
    src/markov/templates/library.cpp
    src/markov/templates/interface.cpp
    src/markov/templates/local.cpp
    src/markov/templates/selection.cpp
    src/markov/templates/validation.cpp
    src/markov/templates/instance.cpp
)
target_include_directories(markov_core PUBLIC src)
target_link_libraries(markov_core PUBLIC nlohmann_json::nlohmann_json)

add_library(markov_sglang
    src/markov/adapters/sglang/templates.cpp
    src/markov/adapters/sglang/workers.cpp
    src/markov/adapters/sglang/actions.cpp
)
target_link_libraries(markov_sglang PUBLIC markov_core)

add_executable(markov src/markov/cli/main.cpp src/markov/cli/serving.cpp src/markov/cli/execution_trace.cpp)
target_link_libraries(markov PRIVATE markov_sglang)
foreach(target markov_core markov_sglang markov)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endforeach()
