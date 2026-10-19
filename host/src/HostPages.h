// Copyright (c) 2026 aopenfx contributors.
//
// Host pages as a device buffer, without a copy.
//
// Only where the device and the host share memory -- Metal on Apple silicon --
// and only for page-aligned mappings, which is what an IOSurface's or an
// mmap's base address is. Everywhere else the answer is zero and `borrow`
// tells its caller to `keep` instead.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aofx::host {

/// A retained backend buffer over `pages` (an `MTL::Buffer*`), or zero.
/// `device` is `gpe::Device::backendDevice()`.
[[nodiscard]] uint64_t wrapHostPages(uint64_t device, const void* pages, size_t bytes);

/// Releases the reference `wrapHostPages` returned.
void releaseWrappedPages(uint64_t buffer);

}   // namespace aofx::host
