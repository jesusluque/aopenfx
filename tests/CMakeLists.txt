# Copyright (c) 2026 aopenfx contributors.
#
# Links aofx and nothing else. That is the test: the SDK claims a plugin can be
# built against it without linking a host, and a test that linked one would not
# be checking it. Default visibility, as a plugin's own build must have, so the
# entry macro's exports are exercised the way a bundle uses them.
add_executable(aofx_sdk_tests test_sdk_headers.cpp)
target_link_libraries(aofx_sdk_tests PRIVATE aofx::aofx)
set_target_properties(aofx_sdk_tests PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF)
add_test(NAME aofx_sdk_tests COMMAND aofx_sdk_tests)

# The build tag: it must name the standard library's ABI and quote this build's
# container sizes. Nothing but the SDK headers is linked.
add_executable(aofx_abi_tag_tests test_abi_tag.cpp)
target_link_libraries(aofx_abi_tag_tests PRIVATE aofx::aofx)
add_test(NAME aofx_abi_tag_tests COMMAND aofx_abi_tag_tests)

# And the case that motivated it: libstdc++ built with the old and the new
# string ABI. Two builds of the same source, and their tags must differ. Only
# where the standard library is libstdc++ -- the switch exists nowhere else.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("
#include <string>
#if !defined(__GLIBCXX__)
#error not libstdc++
#endif
int main() { return 0; }" AOFX_HAVE_LIBSTDCXX)
if(AOFX_HAVE_LIBSTDCXX)
    foreach(abi 0 1)
        add_executable(aofx_abi_tag_cxx11abi${abi} test_abi_tag.cpp)
        target_link_libraries(aofx_abi_tag_cxx11abi${abi} PRIVATE aofx::aofx)
        target_compile_definitions(aofx_abi_tag_cxx11abi${abi}
            PRIVATE _GLIBCXX_USE_CXX11_ABI=${abi})
    endforeach()
    add_test(NAME aofx_abi_tag_differs
             COMMAND ${CMAKE_COMMAND}
                     -DFIRST=$<TARGET_FILE:aofx_abi_tag_cxx11abi0>
                     -DSECOND=$<TARGET_FILE:aofx_abi_tag_cxx11abi1>
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompareTags.cmake)
else()
    message(STATUS "aopenfx: not libstdc++, so the old/new string ABI tag test is skipped")
endif()

# The blur example's regions and reach, checked on a CPU against the same
# functions the effect calls. No kernel, no GPU, no host.
add_executable(aofx_blur_regions_tests test_blur_regions.cpp)
target_link_libraries(aofx_blur_regions_tests PRIVATE aofx::aofx)
target_include_directories(aofx_blur_regions_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/examples/blur)
add_test(NAME aofx_blur_regions_tests COMMAND aofx_blur_regions_tests)
