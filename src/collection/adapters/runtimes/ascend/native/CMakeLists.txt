cmake_minimum_required(VERSION 3.20)
project(markov_native_observation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ASCEND_ROOT /usr/local/Ascend/ascend-toolkit/latest CACHE PATH "Installed CANN SDK")
add_library(markov_native_observation SHARED observation.cpp transfer_sampling.cpp)
target_include_directories(markov_native_observation PRIVATE
    ${ASCEND_ROOT}/include
    ${ASCEND_ROOT}/include/experiment/runtime
    ${ASCEND_ROOT}/include/experiment/runtime/runtime
    ${ASCEND_ROOT}/include/experiment/msprof
    ${ASCEND_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc
    ${ASCEND_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc/runtime
    ${ASCEND_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc/runtime/runtime
    ${ASCEND_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc/profiling
)
target_link_libraries(markov_native_observation PRIVATE dl)
target_compile_options(markov_native_observation PRIVATE -Wall -Wextra -Wpedantic)
