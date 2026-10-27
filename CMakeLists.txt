cmake_minimum_required(VERSION 3.20)
project(markov LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TRACE_GRAPH_DEBUG "Enable detailed graph diagnostics" OFF)
find_package(nlohmann_json REQUIRED)

add_library(markov_source src/markov/trace_graph/source/manifest.cpp)
target_include_directories(markov_source PUBLIC src)
target_link_libraries(markov_source PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(markov_source PRIVATE -Wall -Wextra -Wpedantic)
if(TRACE_GRAPH_DEBUG)
    target_compile_definitions(markov_source PUBLIC TRACE_GRAPH_DEBUG)
endif()

add_executable(markov_core src/markov/main.cpp)
target_link_libraries(markov_core PRIVATE markov_source)
