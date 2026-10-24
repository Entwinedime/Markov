cmake_minimum_required(VERSION 3.20)
project(markov_ascend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

execute_process(
    COMMAND python3 -c "import torch; print(torch.__path__[0]); print(int(torch._C._GLIBCXX_USE_CXX11_ABI))"
    OUTPUT_VARIABLE torch_configuration OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
string(REPLACE "\n" ";" torch_configuration "${torch_configuration}")
list(GET torch_configuration 0 torch_root)
list(GET torch_configuration 1 torch_abi)

# SDK bridges are loaded inside the native process and use that environment's compiler and ABI.
add_library(markov_ascend_observer SHARED native_calls.cpp)
target_include_directories(markov_ascend_observer SYSTEM PRIVATE "${torch_root}/include")
target_link_libraries(markov_ascend_observer PRIVATE "${torch_root}/lib/libc10.so")
target_compile_definitions(markov_ascend_observer PRIVATE _GLIBCXX_USE_CXX11_ABI=${torch_abi})
add_executable(markov_ascend_matmul_selection matmul_selection.cpp)
target_compile_definitions(markov_ascend_matmul_selection PRIVATE _GLIBCXX_USE_CXX11_ABI=0)

foreach(target markov_ascend_observer markov_ascend_matmul_selection)
    target_include_directories(${target} SYSTEM PRIVATE "${MARKOV_ASCEND_INCLUDE_DIR}")
    target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endforeach()
