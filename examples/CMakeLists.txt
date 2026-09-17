# Copyright (c) 2026 aopenfx contributors.
#
# Plugins that need nothing but the SDK, each showing one
# part of it. Bundles land in ${CMAKE_BINARY_DIR}/aofx; point AOFX_PLUGIN_PATH
# at that directory to load them in a host.
include(AofxPlugin)

foreach(example invert crop blur grade merge transform cornerpin generate)
    add_subdirectory(${example})
endforeach()
