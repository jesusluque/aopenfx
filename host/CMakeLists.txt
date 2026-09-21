# Copyright (c) 2026 aopenfx contributors.
#
# The reference host: finding, loading and refusing AOFX bundles, and the host
# side of `aofx::Gpu`. One implementation, linked by every program that hosts
# bundles, with what each program brings declared through `Capabilities`.
#
# Built only when a program adds it: it needs a compute device (gpe) and the
# kernel toolchain gpe finds, and this repository's own build has neither by
# design -- the SDK is headers, and a plugin author needs no host to build one.
#
#   add_subdirectory(<aopenfx>/host <binary dir>)   # after gpe, after sdk/
#
# The target is `aofx::host`. It links `aofx::aofx` (the same headers a plugin
# is built against -- two copies of an interface are two interfaces) and `gpe`,
# whose public headers are the whole of what the host asks of a device.
if(NOT TARGET gpe)
    message(FATAL_ERROR
        "aopenfx/host: add gpe before this directory. The host dispatches "
        "through gpe::PooledDevice and registers kernels with gpe::registerKernel.")
endif()
if(NOT TARGET aofx::aofx)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../sdk" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
endif()

add_library(aofx_host STATIC
    src/Capabilities.cpp
    src/Channels.cpp
    src/EffectRegistry.cpp
    src/EffectRunner.cpp
    src/HostPages.cpp)
add_library(aofx::host ALIAS aofx_host)

target_include_directories(aofx_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(aofx_host PUBLIC cxx_std_20)
target_link_libraries(aofx_host
    PUBLIC  aofx::aofx gpe
    PRIVATE ${CMAKE_DL_LIBS})
set_target_properties(aofx_host PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

if(APPLE)
    # Objective-C++ with ARC: Metal's no-copy buffer takes a block.
    set_source_files_properties(src/HostPages.cpp PROPERTIES
        COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
    target_link_libraries(aofx_host PRIVATE "-framework Metal" "-framework CoreFoundation")
endif()

# The host's own kernel, compiled with the SDK's own build rule. If the way a
# plugin builds a kernel is not good enough for the host, it is not good enough.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../sdk/cmake")
include(AofxKernel)
aofx_add_kernel(aofx_host channels ENTRY channelsMain)
