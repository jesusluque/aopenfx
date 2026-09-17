# Runs two builds of test_abi_tag with --print and requires different tags.
#
#   cmake -DFIRST=<exe> -DSECOND=<exe> -P CompareTags.cmake
execute_process(COMMAND "${FIRST}" --print OUTPUT_VARIABLE first
                RESULT_VARIABLE firstResult OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${SECOND}" --print OUTPUT_VARIABLE second
                RESULT_VARIABLE secondResult OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT firstResult EQUAL 0 OR NOT secondResult EQUAL 0)
    message(FATAL_ERROR "a tag printer failed: ${firstResult} ${secondResult}")
endif()
message(STATUS "first : ${first}")
message(STATUS "second: ${second}")
if(first STREQUAL second)
    message(FATAL_ERROR
        "two builds with different standard-library ABIs wrote the same build "
        "tag; a host could not tell them apart")
endif()
message(STATUS "the tags differ")
