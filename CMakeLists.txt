cmake_minimum_required(VERSION 3.16)
project(LuaVortex VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-fPIC)
    # The vendored VORTEX uses GCC computed-goto (labels-as-values).
    add_compile_options(-fno-strict-aliasing)
endif()

# Suppress some warnings that are noisy in the vendored VORTEX.
add_compile_options(
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-unused-result
    -Wno-format-truncation
    -Wno-stringop-truncation
    -Wno-array-bounds
    -Wno-maybe-uninitialized
    -Wno-discarded-qualifiers
    -Wno-implicit-fallthrough
)

# Build VORTEX as a subproject.
set(VORTEX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(VORTEX_ENABLE_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(VORTEX_ENABLE_SOTA ON CACHE BOOL "" FORCE)
add_subdirectory(vendor/VORTEX)

# LuaVortex include paths.
set(LV_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/VORTEX/src
    ${CMAKE_CURRENT_BINARY_DIR}/vendor/VORTEX
)

# LuaVortex library sources (everything except main).
set(LV_LIB_SOURCES
    src/lv_lexer.c
    src/lv_ast.c
    src/lv_parser.c
    src/lv_codegen.c
    src/lv_value.c
    src/lv_runtime.c
    src/lv_stdlib.c
    src/lv_eval.c
)

# Build a static library so tests and the CLI both link against it.
add_library(luavortex_lib STATIC ${LV_LIB_SOURCES})
target_include_directories(luavortex_lib PUBLIC ${LV_INCLUDE_DIRS})

# Link the VORTEX libraries into luavortex_lib (PUBLIC so consumers inherit).
target_link_libraries(luavortex_lib PUBLIC
    -Wl,--start-group
    vortex_runtime
    vortex_interp
    vortex_baseline
    vortex_ir
    vortex_trace
    vortex_region
    vortex_pea
    vortex_deopt
    vortex_inliner
    vortex_profile
    vortex_lower
    vortex_codecache
    vortex_compile
    vortex_guard
    vortex_midtier
    vortex_sota
    vortex_common
    -Wl,--end-group
    m
    pthread
)

# Main executable.
add_executable(luavortex src/lv_main.c)
target_link_libraries(luavortex PRIVATE luavortex_lib)

# Install (optional).
install(TARGETS luavortex DESTINATION bin)

# Tests
enable_testing()
file(GLOB LV_TEST_SOURCES tests/*.c)
foreach(test_src ${LV_TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} PRIVATE luavortex_lib)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
