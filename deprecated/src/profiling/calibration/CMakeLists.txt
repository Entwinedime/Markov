cmake_minimum_required(VERSION 3.20)
project(runtime_calibration LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_path(ACL_INCLUDE_DIR acl/acl.h HINTS "$ENV{ASCEND_HOME_PATH}/include" REQUIRED)
find_library(ACL_LIBRARY ascendcl HINTS "$ENV{ASCEND_HOME_PATH}/lib64" REQUIRED)
find_library(RUNTIME_LIBRARY runtime HINTS "$ENV{ASCEND_HOME_PATH}/lib64" REQUIRED)

add_executable(idle_stream_sync idle_stream_sync.cpp)
target_include_directories(idle_stream_sync PRIVATE "${ACL_INCLUDE_DIR}")
target_link_libraries(idle_stream_sync PRIVATE "${ACL_LIBRARY}" "${RUNTIME_LIBRARY}")
