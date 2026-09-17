# A generator with no input, and two kernel files: the patterns share one
# parameter struct; the statistics need buffers the patterns do not.
aofx_add_plugin(Generate SOURCES Generate.cpp
    KERNELS "generate ENTRY constantMain ENTRY checkerBoardMain ENTRY colorBarsMain ENTRY colorWheelMain"
            "imagestats ENTRY statsRow ENTRY statsFold ENTRY statsCopy")
