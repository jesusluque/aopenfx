// Copyright (c) 2026 aopenfx contributors.
//
// Compiled as Objective-C++ on Apple platforms (see CMakeLists.txt): the
// no-copy buffer constructor takes a block, which metal-cpp cannot spell.
#include "HostPages.h"

#if defined(__APPLE__)

#import <Metal/Metal.h>

#include <unistd.h>

namespace aofx::host {

uint64_t wrapHostPages(uint64_t device, const void* pages, size_t bytes) {
    id<MTLDevice> metal = (__bridge id<MTLDevice>)reinterpret_cast<void*>(device);
    if (metal == nil || ![metal hasUnifiedMemory]) {
        return 0;   // a discrete GPU would read these pages over the bus
    }
    const auto page = static_cast<size_t>(::getpagesize());
    if (reinterpret_cast<uintptr_t>(pages) % page != 0 || bytes % page != 0) {
        return 0;
    }
    id<MTLBuffer> buffer = [metal newBufferWithBytesNoCopy:const_cast<void*>(pages)
                                                    length:bytes
                                                   options:MTLResourceStorageModeShared
                                               deallocator:nil];
    // +1 from `new`; handed over as an integer, released by the caller.
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>((__bridge_retained void*)buffer));
}

void releaseWrappedPages(uint64_t buffer) {
    if (buffer != 0) {
        CFRelease(reinterpret_cast<CFTypeRef>(buffer));
    }
}

}   // namespace aofx::host

#else

namespace aofx::host {

uint64_t wrapHostPages(uint64_t, const void*, size_t) { return 0; }
void releaseWrappedPages(uint64_t) {}

}   // namespace aofx::host

#endif
