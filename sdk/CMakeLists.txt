# AOFX — the effect SDK.
#
# An INTERFACE target and nothing else. There is no library to link, because a
# plugin that linked the host would not be a plugin: it would share the host's
# allocator, its logging and its dependencies, and every one of those is a way
# for a bundle to take the application down with it.
#
# So the whole SDK is headers, and the only thing crossing the boundary is a
# vtable and some plain structs.
add_library(aofx INTERFACE)
add_library(aofx::aofx ALIAS aofx)

target_include_directories(aofx INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_compile_features(aofx INTERFACE cxx_std_20)
