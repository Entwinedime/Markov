cmake_minimum_required(VERSION 3.20)
project(markov_ascend_calibration LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(ACL_INCLUDE_DIR acl/acl.h HINTS "$ENV{ASCEND_HOME_PATH}/include" REQUIRED)
find_library(ACL_LIBRARY ascendcl HINTS "$ENV{ASCEND_HOME_PATH}/lib64" REQUIRED)

add_executable(markov_stream_sync stream_sync.cpp)
target_include_directories(markov_stream_sync SYSTEM PRIVATE "${ACL_INCLUDE_DIR}")
target_link_libraries(markov_stream_sync PRIVATE "${ACL_LIBRARY}")
target_compile_options(markov_stream_sync PRIVATE -Wall -Wextra -Wpedantic)
