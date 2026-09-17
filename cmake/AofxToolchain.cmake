# Copyright (c) 2026 aopenfx contributors.
#
# The toolchain a kernel is compiled with, found once for the whole build.
#
# `sdk/cmake/AofxKernel.cmake` does not look for compilers itself: inside
# a host's build the compute engine may already have found them and left them in these
# variables. Outside it -- here -- this file finds the same things under the
# same names, so AofxKernel.cmake is the SDK's own file, byte for byte.
#
#   GPE_BACKEND    Metal on Apple, CUDA where a CUDA compiler is found, none
#   GPE_SLANGC     the Slang compiler
#   GPE_METAL      \  Metal: through xcrun, the only stable way to name them
#   GPE_METALLIB   /
#   GPE_NVCC       CUDA: nvcc, which turns Slang's C++ into PTX
#   GPE_CUDA_ARCH  CUDA: the card generation the PTX is built for
#
# And two of its own:
#
#   AOFX_HAVE_KERNELS  whether a plugin with kernels can be built at all
#   AOFX_ARCH_DIR      the bundle's per-platform folder: MacOS, Linux-x86-64, Win64
#
# Pass -DAOFX_GPE_DIR=<gpe checkout> to append gpe's reflection trailer to every
# kernel blob -- the host then refuses a dispatch whose buffer or uniform sizes
# do not match what the kernel declared, instead of rendering nothing.

include_guard(GLOBAL)

# --- where a bundle keeps its binary ---------------------------------------
if(APPLE)
    set(AOFX_ARCH_DIR "MacOS")
elseif(WIN32)
    set(AOFX_ARCH_DIR "Win64")
else()
    set(AOFX_ARCH_DIR "Linux-x86-64")
endif()

# --- which backend ---------------------------------------------------------
if(APPLE)
    set(GPE_BACKEND "Metal")
else()
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        set(GPE_BACKEND "CUDA")
    else()
        set(GPE_BACKEND "none")
    endif()
endif()
message(STATUS "aopenfx: kernel backend ${GPE_BACKEND}")

set(AOFX_HAVE_KERNELS FALSE)

if(NOT GPE_BACKEND STREQUAL "none")
    find_program(GPE_SLANGC slangc
        HINTS $ENV{SLANG_ROOT}/bin $ENV{HOME}/tools/slang/bin
        DOC "Slang compiler")
    if(NOT GPE_SLANGC)
        message(WARNING
            "aopenfx: slangc not found, so no kernel can be compiled and no "
            "example is built. Unpack a release from "
            "github.com/shader-slang/slang and set SLANG_ROOT, or pass "
            "-DGPE_SLANGC=<path>.")
    endif()
endif()

if(GPE_SLANGC AND GPE_BACKEND STREQUAL "Metal")
    # Not in the Command Line Tools and not at a stable path: the Metal compiler
    # is a downloadable toolchain under a versioned mount, and xcrun names it.
    find_program(GPE_XCRUN xcrun REQUIRED)
    execute_process(COMMAND ${GPE_XCRUN} -f metal
                    RESULT_VARIABLE metalFound OUTPUT_QUIET ERROR_QUIET)
    if(NOT metalFound EQUAL 0)
        message(FATAL_ERROR
            "aopenfx: the Metal compiler is not available. It needs full Xcode, "
            "not the Command Line Tools: install Xcode, then "
            "`sudo xcodebuild -license accept && "
            "sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`.")
    endif()
    set(GPE_METAL ${GPE_XCRUN} -sdk macosx metal)
    set(GPE_METALLIB ${GPE_XCRUN} -sdk macosx metallib)
    set(AOFX_HAVE_KERNELS TRUE)
elseif(GPE_SLANGC AND GPE_BACKEND STREQUAL "CUDA")
    get_filename_component(cudaBin "${CMAKE_CUDA_COMPILER}" DIRECTORY)
    find_program(GPE_NVCC nvcc HINTS "${cudaBin}" REQUIRED)
    # A floor, not a ceiling: PTX for one architecture runs on it and everything
    # newer. So the default is the card this machine has; a build that ships
    # bundles to other machines should choose its floor on purpose.
    if(NOT DEFINED GPE_CUDA_ARCH)
        set(GPE_CUDA_ARCH "sm_89")
        find_program(AOFX_NVIDIA_SMI nvidia-smi)
        if(AOFX_NVIDIA_SMI)
            execute_process(
                COMMAND ${AOFX_NVIDIA_SMI} --query-gpu=compute_cap --format=csv,noheader
                OUTPUT_VARIABLE cap RESULT_VARIABLE capResult ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(capResult EQUAL 0 AND cap MATCHES "^([0-9]+)\\.([0-9]+)")
                set(GPE_CUDA_ARCH "sm_${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
            endif()
        endif()
    endif()
    message(STATUS "aopenfx: PTX for ${GPE_CUDA_ARCH}")
    set(AOFX_HAVE_KERNELS TRUE)
endif()

# --- gpe's reflection trailer, when a checkout is named --------------------
set(AOFX_GPE_DIR "" CACHE PATH "A gpe checkout, for the kernel reflection trailer (optional)")
if(AOFX_GPE_DIR AND EXISTS "${AOFX_GPE_DIR}/cmake/KernelTrailer.cmake")
    # The name AofxKernel.cmake reads.
    set(AOFX_HOST_GPE_DIR "${AOFX_GPE_DIR}")
    message(STATUS "aopenfx: kernels carry gpe's reflection trailer")
elseif(AOFX_GPE_DIR)
    message(WARNING "aopenfx: ${AOFX_GPE_DIR} has no cmake/KernelTrailer.cmake")
endif()
