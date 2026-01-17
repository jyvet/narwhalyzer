# Narwhalyzer - GCC Instrumentation Plugin
# CMake Build System
#
# Copyright (c) 2026
# Licensed under GPL-3.0 License

cmake_minimum_required(VERSION 3.16)
project(narwhalyzer
    VERSION 1.0.0
    LANGUAGES C CXX
    DESCRIPTION "GCC plugin for source-level profiling instrumentation"
)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build type defaults to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Options
option(NARWHALYZER_BUILD_EXAMPLES "Build example programs" ON)
option(NARWHALYZER_VERBOSE_BUILD "Enable verbose build output" OFF)

# ============================================================================
# Find GCC Plugin Development Files
# ============================================================================

# Allow user to specify GCC installation
set(GCC_ROOT "" CACHE PATH "Path to GCC installation (optional)")

# Find gcc executable
if(GCC_ROOT)
    find_program(GCC_EXECUTABLE 
        NAMES gcc
        PATHS ${GCC_ROOT}/bin
        NO_DEFAULT_PATH
        REQUIRED
    )
else()
    find_program(GCC_EXECUTABLE 
        NAMES gcc
        REQUIRED
    )
endif()

message(STATUS "Found GCC: ${GCC_EXECUTABLE}")

# Find g++ executable (same installation as gcc)
get_filename_component(GCC_BIN_DIR ${GCC_EXECUTABLE} DIRECTORY)
find_program(GXX_EXECUTABLE 
    NAMES g++
    PATHS ${GCC_BIN_DIR}
    NO_DEFAULT_PATH
)
if(NOT GXX_EXECUTABLE)
    find_program(GXX_EXECUTABLE NAMES g++)
endif()
message(STATUS "Found G++: ${GXX_EXECUTABLE}")

# Get GCC version
execute_process(
    COMMAND ${GCC_EXECUTABLE} -dumpversion
    OUTPUT_VARIABLE GCC_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message(STATUS "GCC version: ${GCC_VERSION}")

# Get GCC plugin include directory
execute_process(
    COMMAND ${GCC_EXECUTABLE} -print-file-name=plugin
    OUTPUT_VARIABLE GCC_PLUGIN_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT EXISTS "${GCC_PLUGIN_DIR}/include")
    message(FATAL_ERROR 
        "GCC plugin development files not found at ${GCC_PLUGIN_DIR}/include\n"
        "Please install GCC plugin development package:\n"
        "  Ubuntu/Debian: sudo apt install gcc-${GCC_VERSION}-plugin-dev\n"
        "  Fedora/RHEL:   sudo dnf install gcc-plugin-devel\n"
        "  Or specify GCC_ROOT to point to your GCC installation."
    )
endif()

set(GCC_PLUGIN_INCLUDE_DIR "${GCC_PLUGIN_DIR}/include")
message(STATUS "GCC plugin include directory: ${GCC_PLUGIN_INCLUDE_DIR}")

# Get GCC's compilation flags for plugins
execute_process(
    COMMAND ${GCC_EXECUTABLE} -print-file-name=
    OUTPUT_VARIABLE GCC_LIBDIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# ============================================================================
# Compiler Flags
# ============================================================================

# Common flags (as a list, not a string)
set(COMMON_FLAGS -Wall -Wextra -fPIC)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND COMMON_FLAGS -g -O0)
else()
    list(APPEND COMMON_FLAGS -O2 -DNDEBUG)
endif()

# Plugin-specific flags (must match GCC's own compilation flags)
set(PLUGIN_CXX_FLAGS -fno-rtti -fno-exceptions)

# ============================================================================
# Runtime Library
# ============================================================================

add_library(narwhalyzer SHARED
    src/narwhalyzer.c
)

target_include_directories(narwhalyzer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(narwhalyzer PRIVATE
    -Wall -Wextra -fPIC
)

target_link_libraries(narwhalyzer PRIVATE
    pthread
)

set_target_properties(narwhalyzer PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    PUBLIC_HEADER "include/narwhalyzer.h"
)

# Also build a static library version
add_library(narwhalyzer_static STATIC
    src/narwhalyzer.c
)

target_include_directories(narwhalyzer_static
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_options(narwhalyzer_static PRIVATE
    -Wall -Wextra
)

target_link_libraries(narwhalyzer_static PRIVATE
    pthread
)

set_target_properties(narwhalyzer_static PROPERTIES
    OUTPUT_NAME narwhalyzer
)

# ============================================================================
# GCC Plugin
# ============================================================================

add_library(narwhalyzer_plugin MODULE
    src/narwhalyzer_pragma_plugin.cc
)

target_include_directories(narwhalyzer_plugin PRIVATE
    ${GCC_PLUGIN_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(narwhalyzer_plugin PRIVATE
    "${COMMON_FLAGS}"
    "${PLUGIN_CXX_FLAGS}"
)

# Plugin must not have lib prefix
set_target_properties(narwhalyzer_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "narwhalyzer"
    SUFFIX ".so"
)

# ============================================================================
# Alternative Plugin Implementation
# ============================================================================

add_library(narwhalyzer_plugin_alt MODULE
    src/narwhalyzer_plugin.cc
)

target_include_directories(narwhalyzer_plugin_alt PRIVATE
    ${GCC_PLUGIN_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(narwhalyzer_plugin_alt PRIVATE
    "${COMMON_FLAGS}"
    "${PLUGIN_CXX_FLAGS}"
)

set_target_properties(narwhalyzer_plugin_alt PROPERTIES
    PREFIX ""
    OUTPUT_NAME "narwhalyzer_alt"
    SUFFIX ".so"
)

# ============================================================================
# Examples
# ============================================================================

if(NARWHALYZER_BUILD_EXAMPLES)
    # Note: Examples are compiled separately with the plugin
    # We just copy them to the build directory
    
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/examples/
         DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/examples/)
endif()

# Create compiler wrapper scripts
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/narwhalyzer-compile.in
    ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer-compile
    @ONLY
)

# Make the script executable and create symlinks
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer-gcc
           ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer-g++
    COMMAND chmod +x ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer-compile
    COMMAND ${CMAKE_COMMAND} -E create_symlink narwhalyzer-compile ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer-gcc
    COMMAND ${CMAKE_COMMAND} -E create_symlink narwhalyzer-compile ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer-g++
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer-compile
    COMMENT "Creating narwhalyzer compiler wrapper symlinks"
)

add_custom_target(compiler_wrappers ALL
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer-gcc
            ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer-g++
)

# ============================================================================
# Installation
# ============================================================================

include(GNUInstallDirs)

# Install runtime library
install(TARGETS narwhalyzer narwhalyzer_static
    EXPORT narwhalyzerTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Install plugin
install(FILES 
    ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer.so
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/gcc/${GCC_VERSION}/plugin
)

# Install CMake config
install(EXPORT narwhalyzerTargets
    FILE narwhalyzerTargets.cmake
    NAMESPACE narwhalyzer::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/narwhalyzer
)

# Create config file
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/narwhalyzerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/narwhalyzer
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/narwhalyzerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/narwhalyzer
)

# ============================================================================
# Testing
# ============================================================================

enable_testing()

# Add a simple test that builds an example
add_test(
    NAME build_example
    COMMAND ${CMAKE_COMMAND} -E env 
        "PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer.so"
        "INCLUDE_PATH=${CMAKE_CURRENT_SOURCE_DIR}/include"
        "RUNTIME_LIB=${CMAKE_CURRENT_BINARY_DIR}/libnarwhalyzer.so"
        ${GCC_EXECUTABLE} 
            -fplugin=${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer.so
            -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -include narwhalyzer.h
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/simple_example.c
            -L${CMAKE_CURRENT_BINARY_DIR}
            -lnarwhalyzer
            -lpthread
            -o ${CMAKE_CURRENT_BINARY_DIR}/simple_test
)

# Add test for nested example
add_test(
    NAME build_nested_example
    COMMAND ${CMAKE_COMMAND} -E env 
        "PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer.so"
        "INCLUDE_PATH=${CMAKE_CURRENT_SOURCE_DIR}/include"
        "RUNTIME_LIB=${CMAKE_CURRENT_BINARY_DIR}/libnarwhalyzer.so"
        ${GCC_EXECUTABLE} 
            -fplugin=${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer.so
            -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -include narwhalyzer.h
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/nested_example.c
            -L${CMAKE_CURRENT_BINARY_DIR}
            -lnarwhalyzer
            -lpthread
            -lm
            -o ${CMAKE_CURRENT_BINARY_DIR}/nested_test
)

# Add test for unstructured region example
add_test(
    NAME build_unstructured_example
    COMMAND ${CMAKE_COMMAND} -E env 
        "PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer.so"
        "INCLUDE_PATH=${CMAKE_CURRENT_SOURCE_DIR}/include"
        "RUNTIME_LIB=${CMAKE_CURRENT_BINARY_DIR}/libnarwhalyzer.so"
        ${GCC_EXECUTABLE} 
            -fplugin=${CMAKE_CURRENT_BINARY_DIR}/narwhalyzer.so
            -I${CMAKE_CURRENT_SOURCE_DIR}/include
            -include narwhalyzer.h
            ${CMAKE_CURRENT_SOURCE_DIR}/examples/unstructured_example.c
            -L${CMAKE_CURRENT_BINARY_DIR}
            -lnarwhalyzer
            -lpthread
            -lm
            -o ${CMAKE_CURRENT_BINARY_DIR}/unstructured_test
)

# ============================================================================
# Summary
# ============================================================================

message(STATUS "")
message(STATUS "Narwhalyzer Configuration Summary")
message(STATUS "==================================")
message(STATUS "  Version:        ${PROJECT_VERSION}")
message(STATUS "  Build type:     ${CMAKE_BUILD_TYPE}")
message(STATUS "  GCC:            ${GCC_EXECUTABLE}")
message(STATUS "  GCC version:    ${GCC_VERSION}")
message(STATUS "  Plugin dir:     ${GCC_PLUGIN_DIR}")
message(STATUS "  Build examples: ${NARWHALYZER_BUILD_EXAMPLES}")
message(STATUS "")
message(STATUS "Build with: cmake --build .")
message(STATUS "")
