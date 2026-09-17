# Two entry points in one blob, and corners read from an attachment a tracker
# hangs on its picture. Two plugins from one source: CornerPin and CornerPinSS.
aofx_add_plugin(CornerPin SOURCES CornerPin.cpp KERNELS "cornerpin ENTRY pinWarp ENTRY pinBlur")
