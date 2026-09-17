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
