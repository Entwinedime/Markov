cmake_minimum_required(VERSION 3.20)
project(markov_ascend_calibration LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(ACL_INCLUDE_DIR acl/acl.h HINTS "$ENV{ASCEND_HOME_PATH}/include" REQUIRED)
find_library(ACL_LIBRARY ascendcl HINTS "$ENV{ASCEND_HOME_PATH}/lib64" REQUIRED)

add_executable(markov_runtime_services runtime_services.cpp)
target_include_directories(markov_runtime_services SYSTEM PRIVATE "${ACL_INCLUDE_DIR}")
target_link_libraries(markov_runtime_services PRIVATE "${ACL_LIBRARY}")
target_compile_options(markov_runtime_services PRIVATE -Wall -Wextra -Wpedantic)
