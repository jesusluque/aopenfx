# Copyright (c) 2026 aopenfx contributors.
#
# One AOFX plugin, as a bundle.
#
#   aofx_add_plugin(<Name>
#       SOURCES <file.cpp>...
#       KERNELS "<slang name> ENTRY <entry> [ENTRY <entry>...]" ...)
#
# Builds <Name>.aofx.bundle/Contents/<AOFX_ARCH_DIR>/<Name>.aofx under
# ${CMAKE_BINARY_DIR}/aofx, one kernel blob per KERNELS item (each is the
# argument list of aofx_add_kernel after the target), and on Apple copies the
# directory's Info.plist into Contents/. The target is aofx_<name lowercase>.

include_guard(GLOBAL)
include(AofxToolchain)
include(AofxKernel)

function(aofx_add_plugin name)
    cmake_parse_arguments(ARG "" "" "SOURCES;KERNELS" ${ARGN})
    string(TOLOWER "${name}" lower)
    set(target "aofx_${lower}")
    if(NOT AOFX_HAVE_KERNELS)
        message(STATUS "aopenfx: ${name} skipped, no kernel toolchain")
        return()
    endif()

    set(bundle "${CMAKE_BINARY_DIR}/aofx/${name}.aofx.bundle")
    add_library(${target} MODULE ${ARG_SOURCES})
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${name}"
        PREFIX ""
        SUFFIX ".aofx"
        LIBRARY_OUTPUT_DIRECTORY "${bundle}/Contents/${AOFX_ARCH_DIR}"
        # Windows puts a MODULE's DLL under RUNTIME for some generators.
        RUNTIME_OUTPUT_DIRECTORY "${bundle}/Contents/${AOFX_ARCH_DIR}"
        # The four exported symbols must stay visible. AOFX_EXPORT_EFFECTS marks
        # them itself (dllexport on Windows); this keeps a project-wide hidden
        # default from ever reaching a plugin.
        CXX_VISIBILITY_PRESET default
        VISIBILITY_INLINES_HIDDEN OFF)

    # The SDK headers and nothing else. A plugin that linked the host would share
    # its allocator, its logging and its dependencies.
    target_link_libraries(${target} PRIVATE aofx::aofx)

    foreach(kernel IN LISTS ARG_KERNELS)
        separate_arguments(kernelArgs UNIX_COMMAND "${kernel}")
        aofx_add_kernel(${target} ${kernelArgs})
    endforeach()

    if(APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist")
        configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
                       "${bundle}/Contents/Info.plist" COPYONLY)
    endif()
endfunction()
