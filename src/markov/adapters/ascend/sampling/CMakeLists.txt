cmake_minimum_required(VERSION 3.20)
project(markov_ascend_sampling LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(MARKOV_ASCEND_ROOT "" CACHE PATH "Installed CANN architecture directory")

find_path(ASCEND_INCLUDE acl/acl.h PATHS "${MARKOV_ASCEND_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
foreach(name ascendcl opapi nnopbase)
    find_library(${name}_LIBRARY ${name} PATHS "${MARKOV_ASCEND_ROOT}/lib64" NO_DEFAULT_PATH REQUIRED)
endforeach()

add_executable(markov_index_put_sample index_put.cpp)
target_include_directories(markov_index_put_sample SYSTEM PRIVATE "${ASCEND_INCLUDE}")
target_link_libraries(markov_index_put_sample PRIVATE ${opapi_LIBRARY} ${nnopbase_LIBRARY} ${ascendcl_LIBRARY})
target_compile_options(markov_index_put_sample PRIVATE -Wall -Wextra -Wpedantic)
