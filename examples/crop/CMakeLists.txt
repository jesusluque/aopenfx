# Regions: a region of definition that is the box when it cuts, and a region of
# interest that asks the input for only that.
aofx_add_plugin(Crop SOURCES Crop.cpp KERNELS "crop ENTRY cropMain")
