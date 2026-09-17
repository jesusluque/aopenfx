# Roadmap

Ideas that are designed or wanted and not scheduled. None of these is a
commitment; each says why it is here.

## A versioned C interface, with a C++ layer on top

**Why.** The interface is C++ across the boundary -- virtual functions,
`std::string`, `std::vector` -- so a bundle must be built with the host's
compiler and standard library, and `buildTag()` exists to refuse the ones that
are not. That is acceptable for plugins built beside the host and a real barrier
for third parties shipping binaries.

**Shape.** A C ABI of opaque handles (`aofx_effect*`, `aofx_request*`,
`aofx_buffer`) and plain functions, versioned per function table with a size
field, so a newer host can read an older table. The current C++ classes become a
header-only layer that compiles against the C tables inside the plugin, so
plugin source keeps its shape. `buildTag()` then stops being a gate and becomes
a diagnostic.
