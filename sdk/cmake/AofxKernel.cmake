# Copyright (c) 2026 aopenfx contributors.
#
# Compiling a plugin's kernels and embedding them in its bundle.
#
# A kernel is written once, in Slang, and compiled at build time for whichever
# backend this build targets: PTX for CUDA, a metallib for Metal. The result is
# a byte array in the plugin's own binary, not a file beside it -- a bundle with
# loose kernels is a bundle to sign, to install, and to have half of.
#
#   aofx_add_kernel(<target> <name> ENTRY <entryPoint> [ENTRY <entryPoint>...]
#                   [SOURCE <file.slang>])
#
# More than one entry point compiles into a single blob. That is not a
# convenience: entry points in one file share their buffer declarations and
# their parameter struct, and a multi-pass effect whose passes were compiled
# separately would be several copies of one layout, free to drift apart. The
# host registers the blob once and `Gpu::load` finds each entry by name.
#
# Compiles <name>.slang from the current source directory -- or SOURCE, for a
# kernel that lives under a subdirectory of its own -- and generates
# aofx_kernels_<name>.h next to it in the build tree, holding
# `k_<name>` and `k_<name>Bytes`. Include it and hand both to KernelDesc.
#
# The toolchain is the one gpe already found. Building a plugin outside this
# tree means setting these yourself -- which is the same requirement as the C++
# ABI: an AOFX plugin is built with the host's toolchain or not at all.

# The reflection trailer's generator, beside this file; see AofxKernelTrailer.cmake.
# A cache entry rather than a directory variable: the function below is global
# once this file is included anywhere, and a program that includes it once and
# calls aofx_add_kernel from other directories must still find the generator.
set(_AOFX_KERNEL_TRAILER "${CMAKE_CURRENT_LIST_DIR}/AofxKernelTrailer.cmake"
    CACHE INTERNAL "Where aofx_add_kernel finds the kernel trailer generator")
option(AOFX_KERNEL_REFLECTION
       "Append a reflection trailer (thread groups, buffer and uniform sizes) to every kernel" ON)

function(aofx_add_kernel target name)
    cmake_parse_arguments(ARG "" "SOURCE" "ENTRY" ${ARGN})
    if(NOT ARG_ENTRY)
        message(FATAL_ERROR "aofx_add_kernel(${name}) needs ENTRY")
    endif()
    # `-stage` binds to the entry point before it, so the pair has to repeat.
    set(entryFlags "")
    foreach(entry IN LISTS ARG_ENTRY)
        list(APPEND entryFlags -entry ${entry} -stage compute)
    endforeach()
    if(NOT GPE_SLANGC)
        message(FATAL_ERROR
            "aofx_add_kernel(${name}): no Slang compiler. This build has no "
            "compute backend, so there is nothing to compile a kernel for.")
    endif()

    if(ARG_SOURCE)
        get_filename_component(source "${ARG_SOURCE}" ABSOLUTE
                               BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/${name}.slang")
    endif()
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/kernels")
    file(MAKE_DIRECTORY "${dir}")
    set(blob "${dir}/${name}.blob")
    set(json "${dir}/${name}.json")
    set(header "${dir}/aofx_kernels_${name}.h")

    if(GPE_BACKEND STREQUAL "Metal")
        set(msl "${dir}/${name}.metal")
        set(air "${dir}/${name}.air")
        add_custom_command(
            OUTPUT "${blob}"
            COMMAND ${GPE_SLANGC} "${source}" -target metal
                    ${entryFlags} -o "${msl}"
                    -reflection-json "${json}"
            COMMAND ${GPE_METAL} -c "${msl}" -o "${air}"
            COMMAND ${GPE_METALLIB} "${air}" -o "${blob}"
            DEPENDS "${source}"
            COMMENT "slang -> metallib: ${name}"
            VERBATIM)
    elseif(GPE_BACKEND STREQUAL "CUDA")
        set(cu "${dir}/${name}.cu")
        # -ptx rather than -cubin: PTX is forward compatible, so a plugin built
        # today runs on a card that does not exist yet. The driver JITs it once
        # and caches the result.
        add_custom_command(
            OUTPUT "${blob}"
            COMMAND ${GPE_SLANGC} "${source}" -target cuda
                    ${entryFlags} -o "${cu}"
                    -reflection-json "${json}"
            COMMAND ${GPE_NVCC} -ptx "${cu}" -o "${blob}" -arch=${GPE_CUDA_ARCH}
            DEPENDS "${source}"
            COMMENT "slang -> ptx: ${name}"
            VERBATIM)
    else()
        message(FATAL_ERROR "aofx_add_kernel(${name}): no backend to compile for")
    endif()

    # Written by a script at build time rather than by configure_file: the blob
    # does not exist at configure time, and a header generated from a file that
    # is not there yet is a header full of nothing.
    set(script "${dir}/write_${name}_header.cmake")
    # WHAT THE KERNEL KNOWS ABOUT ITSELF, CARRIED WITH IT
    #
    # slangc already knows a kernel's [numthreads], the size of one element of
    # each of its buffers and how many bytes its uniform block has. Until this,
    # none of that reached the backend: Metal launched 16x16 groups whatever
    # the kernel said, and a dispatch handed a uniform block of the wrong size
    # bound it anyway and rendered something. That last one is the reason a
    # .slang file and its mirror in C++ drifting apart used to produce black
    # output and not one word.
    #
    # A host reads a trailer appended to the blob and refuses, by name, a
    # dispatch that does not match. The generator is this SDK's own
    # (AofxKernelTrailer.cmake), so every kernel carries one unless the build
    # turns it off with -DAOFX_KERNEL_REFLECTION=OFF.
    set(withTrailer "")
    if(AOFX_KERNEL_REFLECTION)
        set(withTrailer "\
include(\"${_AOFX_KERNEL_TRAILER}\")
aofx_kernel_trailer_hex(\"${json}\" \"${ARG_ENTRY}\" trailer)
string(APPEND hex \"\${trailer}\")
")
    endif()
    file(WRITE "${script}" "\
file(READ \"${blob}\" hex HEX)
${withTrailer}string(REGEX REPLACE \"(..)\" \"0x\\\\1,\" bytes \"\${hex}\")
string(REGEX REPLACE \"((0x..,){12})\" \"\\\\1\\n    \" bytes \"\${bytes}\")
file(WRITE \"${header}\" \"\
// Generated at build time from ${name}.slang. Do not edit.
#pragma once

#include <cstddef>

inline constexpr unsigned char k_${name}[] = {
    \${bytes}
};
inline constexpr size_t k_${name}Bytes = sizeof(k_${name});
\")
")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND ${CMAKE_COMMAND} -P "${script}"
        DEPENDS "${blob}" "${script}"
        COMMENT "embedding kernel: ${name}"
        VERBATIM)

    target_sources(${target} PRIVATE "${header}")
    target_include_directories(${target} PRIVATE "${dir}")
endfunction()
