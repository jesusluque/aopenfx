# Copyright (c) 2026 aopenfx contributors.
#
# What a kernel knows about itself, carried with it.
#
# slangc knows a kernel's [numthreads], the size of one element of each of its
# structured buffers and how many bytes its uniform block has, and says so with
# -reflection-json. A host that has those numbers can refuse a dispatch that
# disagrees with them -- the wrong number of buffers, a uniform struct whose C++
# mirror drifted from the .slang one -- instead of running it and rendering
# something wrong. So they are appended to the embedded blob as a trailer, which
# the host strips before any backend sees the bytes: a Metal library and a PTX
# module both reject anything after their own end.
#
# THE LAYOUT, every word a uint32, little-endian:
#
#     for each entry point, in the order given:
#         nameLength, name bytes padded with zeros to a multiple of 4,
#         groupX, groupY, groupZ
#     bufferCount, elementBytes x bufferCount, uniformBytes,
#     entryCount, trailerBytes, version (1), 'G' 'P' 'E' 'K'
#
# `trailerBytes` counts the whole trailer, its last four words included, so a
# reader finds the start from the end. The magic and version are the ones gpe's
# kernel registry reads.
#
# A kernel whose parameters are anything but structured buffers and at most one
# constant buffer -- a texture, a sampler, a loose uniform -- gets no trailer,
# and runs as a blob without one always has.
#
#   aofx_kernel_trailer_hex(<reflection json> "<entry;entry>" <out var>)
#
# Sets <out var> to the trailer as lowercase hex, or to "" when there is none.
# Safe in CMake script mode (-P), which is where the header generator runs.

include_guard(GLOBAL)

# One unsigned 32-bit word, as eight hex digits in little-endian byte order.
function(_aofx_word_hex value out)
    math(EXPR big "${value}" OUTPUT_FORMAT HEXADECIMAL)   # "0x..."
    string(SUBSTRING "${big}" 2 -1 big)
    string(LENGTH "${big}" digits)
    if(digits GREATER 8)
        message(FATAL_ERROR "aofx_kernel_trailer_hex: ${value} does not fit in 32 bits")
    endif()
    while(digits LESS 8)
        string(PREPEND big "0")
        math(EXPR digits "${digits} + 1")
    endwhile()
    string(TOLOWER "${big}" big)
    set(little "")
    foreach(at 6 4 2 0)
        string(SUBSTRING "${big}" ${at} 2 byte)
        string(APPEND little "${byte}")
    endforeach()
    set(${out} "${little}" PARENT_SCOPE)
endfunction()

function(aofx_kernel_trailer_hex json_file entries out)
    set(${out} "" PARENT_SCOPE)
    if(NOT EXISTS "${json_file}")
        return()
    endif()
    file(READ "${json_file}" json)

    # The parameters: structured buffers in declaration order, and the uniform
    # block. Anything else and there is nothing this layout can say.
    string(JSON params ERROR_VARIABLE bad LENGTH "${json}" parameters)
    if(bad)
        return()
    endif()
    set(elementWords "")
    set(bufferCount 0)
    set(uniformBytes 0)
    set(constantBuffers 0)
    if(params GREATER 0)
        math(EXPR lastParam "${params} - 1")
        foreach(p RANGE ${lastParam})
            string(JSON kind ERROR_VARIABLE bad GET "${json}" parameters ${p} type kind)
            if(bad)
                return()
            endif()
            if(kind STREQUAL "resource")
                string(JSON shape ERROR_VARIABLE bad
                       GET "${json}" parameters ${p} type baseShape)
                if(bad OR NOT shape STREQUAL "structuredBuffer")
                    return()
                endif()
                string(JSON elementBytes ERROR_VARIABLE bad
                       GET "${json}" parameters ${p} type resultType sizes 0 value)
                if(bad)
                    return()
                endif()
                _aofx_word_hex(${elementBytes} word)
                string(APPEND elementWords "${word}")
                math(EXPR bufferCount "${bufferCount} + 1")
            elseif(kind STREQUAL "constantBuffer")
                string(JSON uniformBytes ERROR_VARIABLE bad
                       GET "${json}" parameters ${p} type elementType sizes 0 value)
                if(bad)
                    return()
                endif()
                math(EXPR constantBuffers "${constantBuffers} + 1")
            else()
                return()
            endif()
        endforeach()
    endif()
    if(constantBuffers GREATER 1)
        return()
    endif()

    # The entry points, in the order the caller named them.
    string(JSON entryPoints ERROR_VARIABLE bad LENGTH "${json}" entryPoints)
    if(bad OR entryPoints EQUAL 0)
        return()
    endif()
    math(EXPR lastEntry "${entryPoints} - 1")
    set(trailer "")
    set(entryCount 0)
    foreach(entry IN LISTS entries)
        set(group "")
        foreach(e RANGE ${lastEntry})
            string(JSON name GET "${json}" entryPoints ${e} name)
            if(name STREQUAL entry)
                foreach(axis 0 1 2)
                    string(JSON size GET "${json}" entryPoints ${e} threadGroupSize ${axis})
                    list(APPEND group ${size})
                endforeach()
                break()
            endif()
        endforeach()
        if(NOT group)
            return()   # an entry the reflection does not know: say nothing
        endif()
        string(LENGTH "${entry}" nameBytes)
        _aofx_word_hex(${nameBytes} word)
        string(APPEND trailer "${word}")
        string(HEX "${entry}" nameHex)
        math(EXPR padding "(4 - ${nameBytes} % 4) % 4")
        string(REPEAT "00" ${padding} zeros)
        string(APPEND trailer "${nameHex}${zeros}")
        foreach(size IN LISTS group)
            _aofx_word_hex(${size} word)
            string(APPEND trailer "${word}")
        endforeach()
        math(EXPR entryCount "${entryCount} + 1")
    endforeach()

    _aofx_word_hex(${bufferCount} word)
    string(APPEND trailer "${word}${elementWords}")
    _aofx_word_hex(${uniformBytes} word)
    string(APPEND trailer "${word}")
    _aofx_word_hex(${entryCount} word)
    string(APPEND trailer "${word}")

    # The three words still to come -- trailerBytes, version, magic -- are part
    # of what trailerBytes counts.
    string(LENGTH "${trailer}" hexDigits)
    math(EXPR trailerBytes "${hexDigits} / 2 + 12")
    _aofx_word_hex(${trailerBytes} word)
    string(APPEND trailer "${word}")
    _aofx_word_hex(1 word)
    string(APPEND trailer "${word}")
    string(HEX "GPEK" magic)
    string(APPEND trailer "${magic}")
    set(${out} "${trailer}" PARENT_SCOPE)
endfunction()
