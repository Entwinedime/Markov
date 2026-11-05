cmake_minimum_required(VERSION 3.20)
project(markov_ascend_profiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/../../torch/lib")

execute_process(
    COMMAND python3 -c "import importlib.util, torch; print(torch.__path__[0]); print(importlib.util.find_spec('torch_npu').submodule_search_locations[0])"
    OUTPUT_VARIABLE sdk_configuration OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
string(REPLACE "\n" ";" sdk_configuration "${sdk_configuration}")
list(GET sdk_configuration 0 torch_root)
list(GET sdk_configuration 1 torch_npu_root)

# Rebuild the upstream component against the installed SDK headers and Torch ABI.
find_package(Torch REQUIRED PATHS "${torch_root}/share/cmake/Torch")
add_subdirectory("${MARKOV_TORCH_NPU_SOURCE_DIR}/torch_npu/csrc/toolkit/profiler" profiler)
target_include_directories(npu_profiler SYSTEM PRIVATE "${torch_npu_root}/include" ${TORCH_INCLUDE_DIRS})
target_compile_options(npu_profiler PRIVATE -O2 -fstack-protector-all)
target_link_options(npu_profiler PRIVATE -Wl,-z,relro,-z,now,-z,noexecstack)
