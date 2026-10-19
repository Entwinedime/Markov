cmake_minimum_required(VERSION 3.20)
project(markov_native_observation LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ASCEND_ROOT /usr/local/Ascend/ascend-toolkit/latest CACHE PATH "Installed CANN SDK")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} -c "from importlib.util import find_spec; print(find_spec('torch').submodule_search_locations[0] + '/share/cmake')"
    OUTPUT_VARIABLE TORCH_CMAKE_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
list(APPEND CMAKE_PREFIX_PATH "${TORCH_CMAKE_PREFIX}")
find_package(Torch REQUIRED)

set(COLLECTION_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
add_library(markov_cpu_sampling SHARED
    ${COLLECTION_ROOT}/instrumentation/native/cpu_sampling.cpp
    ${COLLECTION_ROOT}/adapters/runtimes/pytorch/native/cpu_sampling.cpp)
target_include_directories(markov_cpu_sampling PUBLIC ${COLLECTION_ROOT}/instrumentation/native)
target_link_libraries(markov_cpu_sampling PRIVATE ${TORCH_LIBRARIES})
separate_arguments(TORCH_COMPILER_FLAGS NATIVE_COMMAND "${TORCH_CXX_FLAGS}")
target_compile_options(markov_cpu_sampling PRIVATE -Wall -Wextra -Wpedantic ${TORCH_COMPILER_FLAGS})

add_library(markov_native_observation SHARED observation.cpp transfer_sampling.cpp cpu_launches.cpp)
target_include_directories(markov_native_observation PRIVATE
    ${COLLECTION_ROOT}/../markov/include
    ${ASCEND_ROOT}/include
    ${ASCEND_ROOT}/include/experiment/runtime
    ${ASCEND_ROOT}/include/experiment/runtime/runtime
    ${ASCEND_ROOT}/include/experiment/msprof
    ${ASCEND_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc
    ${ASCEND_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc/runtime
    ${ASCEND_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc/runtime/runtime
    ${ASCEND_ROOT}/${CMAKE_SYSTEM_PROCESSOR}-linux/pkg_inc/profiling
)
target_link_libraries(markov_native_observation PRIVATE markov_cpu_sampling dl)
target_compile_options(markov_native_observation PRIVATE -Wall -Wextra -Wpedantic)
