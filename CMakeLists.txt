cmake_minimum_required(VERSION 3.20)
project(ktransformers_sglang_trace_sim VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# C++ 侧保留 LD_PRELOAD hook，并重新启用面向对象 TraceGraph 作为 base DAG 引擎。
add_subdirectory(src/profiling/ld_preload)
add_subdirectory(src/modeling/trace_graph)

install(TARGETS hook
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(TARGETS trace_graph
    RUNTIME DESTINATION bin
)
