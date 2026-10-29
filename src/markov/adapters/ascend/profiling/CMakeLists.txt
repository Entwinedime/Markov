cmake_minimum_required(VERSION 3.20)
project(markov_ascend_profiling LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(ACL_INCLUDE_DIR acl/acl.h HINTS "$ENV{ASCEND_HOME_PATH}/include" REQUIRED)
find_package(Threads REQUIRED)
add_library(markov_runtime_probe SHARED runtime_probe.cpp)
target_include_directories(markov_runtime_probe SYSTEM PRIVATE "${ACL_INCLUDE_DIR}")
target_link_libraries(markov_runtime_probe PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
target_compile_options(markov_runtime_probe PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS markov_runtime_probe LIBRARY DESTINATION lib)
