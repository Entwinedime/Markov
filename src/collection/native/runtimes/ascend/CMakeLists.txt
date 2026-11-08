cmake_minimum_required(VERSION 3.20)
project(markov_ascend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The bridge consumes only the SDK C ABI; model-selection and Torch workspace code are not part of observation.
add_library(markov_ascend_observer SHARED native_calls.cpp)
target_include_directories(markov_ascend_observer SYSTEM PRIVATE "${MARKOV_ASCEND_INCLUDE_DIR}")
target_link_libraries(markov_ascend_observer PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(markov_ascend_observer PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
