cmake_minimum_required(VERSION 3.20)
project(hook_framework LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HOOK_ENABLE_PAPI "Enable PAPI PMU support" ON)
option(HOOK_USE_ASCENDCL_TARGETS "Build AscendCL hook wrappers into libhook.so" ON)

set(HOOK_PROFILE "ascendcl" CACHE STRING "Hook wrapper profile: template, ascendcl, sglang, or ktransformers.")
set_property(CACHE HOOK_PROFILE PROPERTY STRINGS template ascendcl sglang ktransformers)
set(HOOK_TARGET_SOURCE "" CACHE STRING "Custom hook wrapper source file. Overrides HOOK_USE_ASCENDCL_TARGETS when set.")

if(HOOK_TARGET_SOURCE)
    set(HOOK_WRAPPER_SOURCE ${HOOK_TARGET_SOURCE})
elseif(NOT HOOK_USE_ASCENDCL_TARGETS)
    set(HOOK_WRAPPER_SOURCE hook.cpp)
elseif(HOOK_PROFILE STREQUAL "template")
    set(HOOK_WRAPPER_SOURCE hook.cpp)
elseif(HOOK_PROFILE STREQUAL "sglang")
    set(HOOK_WRAPPER_SOURCE targets/sglang_hooks.cpp)
elseif(HOOK_PROFILE STREQUAL "ktransformers")
    set(HOOK_WRAPPER_SOURCE targets/ktransformers_hooks.cpp)
elseif(HOOK_PROFILE STREQUAL "ascendcl")
    set(HOOK_WRAPPER_SOURCE targets/ascendcl_hooks.cpp)
else()
    message(FATAL_ERROR "Unknown HOOK_PROFILE: ${HOOK_PROFILE}")
endif()

message(STATUS "Hook profile: ${HOOK_PROFILE}")
message(STATUS "Hook wrapper source: ${HOOK_WRAPPER_SOURCE}")

add_library(hook SHARED
    ${HOOK_WRAPPER_SOURCE}
    src/common.cpp
    src/pmu_recorder.cpp
    src/relation_rules.cpp
    src/scope_registry.cpp
    src/symbol_resolver.cpp
    src/trace_logger.cpp
)

target_include_directories(hook
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hook
    PRIVATE
        dl
)

target_compile_definitions(hook
    PRIVATE
        _GNU_SOURCE
        HOOK_PROFILE_NAME="${HOOK_PROFILE}"
)

if(HOOK_ENABLE_PAPI)
    find_library(PAPI_LIBRARY NAMES papi)
    if(PAPI_LIBRARY)
        target_compile_definitions(hook PRIVATE HOOK_ENABLE_PAPI)
        target_link_libraries(hook PRIVATE ${PAPI_LIBRARY})
    else()
        message(WARNING "HOOK_ENABLE_PAPI is ON but libpapi was not found. Building without PAPI.")
    endif()
endif()

set_target_properties(hook PROPERTIES
    OUTPUT_NAME hook
)
