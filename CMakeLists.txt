# Copyright (c) 2026 aopenfx contributors.
#
# aopenfx: the AOFX effect SDK, on its own.
#
# The SDK is headers (sdk/include/aofx) and one CMake function that compiles a
# Slang kernel into the plugin's binary (sdk/cmake/AofxKernel.cmake). This
# project adds what building a plugin on its own needs -- finding the kernel
# toolchain, and turning a plugin into a bundle -- plus examples and the header
# test. Nothing here links a host.
cmake_minimum_required(VERSION 3.24)
project(aopenfx VERSION 26 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake")

include(AofxToolchain)

add_subdirectory(sdk)

option(AOFX_BUILD_EXAMPLES "Build the example plugins" ON)
if(AOFX_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

option(AOFX_BUILD_TESTS "Build the SDK header test" ON)
if(AOFX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
