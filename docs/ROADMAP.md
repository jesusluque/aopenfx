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

## Kernel reflection built into the SDK

**Why.** `aofx_add_kernel` appends a reflection trailer to each kernel blob --
thread-group size, buffer element sizes, uniform block size -- only when
`AOFX_GPE_DIR` names a gpe checkout, because the generator lives there. A host
uses the trailer to refuse a dispatch that disagrees with the kernel instead of
rendering black. It should be on by default, with no second checkout.

**Blocked on a decision.** Either vendor gpe's `KernelTrailer.cmake` into
`sdk/cmake/` or rewrite the generator here from `slangc -reflection-json`. Both
need the licence question settled first. Until then the configure warns when
the trailer is missing, and `-DAOFX_REQUIRE_REFLECTION=ON` makes it an error.

## The life cycle of what `keep()` holds

**Why.** `Gpu::keep()` holds device memory "until `drop` or the end of the
process", with no eviction. The key an effect uses for per-node state is
`request.instance`, the node's path, which changes when a node is renamed or
moved between groups. Today the old buffer is stranded until the process ends,
a deleted node's buffers are never released, and one plugin can hold any amount
of device memory. On a long-running host (a render service, a playout desk)
all three accumulate.

**Proposal.** Four pieces, each usable without the others. None is implemented;
each one that changes a struct or the vtable needs its own ABI bump, and the
vtable changes need approval first.

1. **A stable instance identifier.** `RenderRequest::instanceId`, an opaque
   64-bit value the host assigns when a node is created and keeps across
   renames, group moves and document reloads (stored in the document). The path
   stays for display; keys use the id. A rename then changes nothing an effect
   holds.
2. **Notification that a node went away.** `Effect::forget(uint64_t instanceId)`,
   a new virtual with an empty default, called by the host when a node is
   deleted or its document closes. The host also drops every kept key the
   effect registered under that id (see 3), so an effect that ignores the call
   still does not leak.
3. **A memory budget per plugin.** `keep` gains an owner: the host records which
   plugin and which instance made each key (from the request the call arrives
   with), counts device bytes per plugin, and exposes them
   (`Gpu::keptBytes()` for the effect, and in the host's own diagnostics). A
   declared budget, `EffectDesc::keepBudgetBytes` (zero meaning "host
   default"), bounds it.
4. **An eviction policy.** Over budget, the host evicts least-recently-used keys
   of that plugin, never keys touched by a render still in flight. Eviction is
   what a key's absence already means -- `keep(key, nullptr, 0)` returns
   invalid and the effect rebuilds -- so effects that follow today's rule
   ("make the first frame after a reset produce something sensible") need no
   change. Keys an effect cannot rebuild are marked at `keep` time
   (`KeepFlags::Pinned`) and count against the budget but are never evicted.

**Order.** 2 and 3 first: they fix the leaks and make the problem measurable
without touching any effect. 1 next, with a document-format change in the host.
4 last, once budgets have real numbers behind them.

## Smaller buffers

**Why.** A `Buffer` is always RGBA float32, linear, premultiplied: about 33 MB
at 1080p, 133 MB at UHD, 531 MB at 8K. Mattes, masks and depth are one channel,
and many intermediates would be fine at half precision.

**Idea.** A per-plane format (float16, single channel) declared by the effect
for its outputs and scratches, with the host converting at the boundary where a
consumer needs the full format. Not designed yet; it touches every kernel's
indexing and the host's cache.
