# The smallest plugin: one input, one kernel, no parameters worth the name.
aofx_add_plugin(Invert SOURCES Invert.cpp KERNELS "invert ENTRY invertMain")
