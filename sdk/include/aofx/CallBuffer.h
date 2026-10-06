// Copyright (c) 2026 aopenfx contributors.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "aofx/Effect.h"

namespace aofx {

/// A buffer for this one call: data that changes every frame -- a control
/// block for a model, this frame's dots or segments -- on the card for as long
/// as the call lasts, and dropped when it goes out of scope.
///
/// `Gpu::keep` is keyed by contents and shared (see its comment), so the
/// obvious key for such data -- the effect's name and the node's instance --
/// is wrong as soon as one node renders two frames at once: the prefetcher,
/// a second viewer, a host that renders frames side by side. Both calls drop
/// and re-keep the same key, and one of them hands its kernel or its model the
/// other's numbers. The key here carries a number no other call has, so two
/// calls of one node never meet.
///
/// Dropping at the end of the call is safe while a kernel bound to the buffer
/// is still queued: a host frees a dropped buffer only once the device is done
/// with the work that uses it. What is not safe is keeping the Buffer handle
/// past this object -- copy the data out first if it must outlive the call.
class CallBuffer {
public:
    explicit CallBuffer(Gpu* gpu) : gpu_(gpu) {}
    CallBuffer(Gpu* gpu, std::string_view what, const std::string& instance, const void* data,
               size_t bytes)
        : gpu_(gpu) {
        keep(what, instance, data, bytes);
    }
    ~CallBuffer() { release(); }

    CallBuffer(const CallBuffer&) = delete;
    CallBuffer& operator=(const CallBuffer&) = delete;
    CallBuffer(CallBuffer&& other) noexcept
        : gpu_(other.gpu_), key_(std::move(other.key_)), buffer_(other.buffer_) {
        other.key_.clear();
        other.buffer_ = Buffer{};
    }
    CallBuffer& operator=(CallBuffer&& other) noexcept {
        if (this != &other) {
            release();
            gpu_ = other.gpu_;
            key_ = std::move(other.key_);
            buffer_ = other.buffer_;
            other.key_.clear();
            other.buffer_ = Buffer{};
        }
        return *this;
    }

    /// Puts `bytes` of `data` on the card under a key of this call's own,
    /// dropping whatever this object held before. `bytes` must be a multiple
    /// of sixteen, as for `Gpu::keep`. An invalid Buffer when the card had no
    /// room or there is no device.
    const Buffer& keep(std::string_view what, const std::string& instance, const void* data,
                       size_t bytes) {
        release();
        if (gpu_ == nullptr || data == nullptr || bytes == 0) {
            return buffer_;
        }
        key_.assign(what);
        key_ += '.';
        key_ += instance;
        key_ += ".call";
        key_ += std::to_string(next().fetch_add(1, std::memory_order_relaxed));
        buffer_ = gpu_->keep(key_, data, bytes);
        if (!buffer_.isValid()) {
            gpu_->drop(key_);
            key_.clear();
        }
        return buffer_;
    }

    [[nodiscard]] const Buffer& buffer() const { return buffer_; }
    [[nodiscard]] bool isValid() const { return buffer_.isValid(); }

private:
    /// One count per effect binary, which is all a key needs: the effect's
    /// name and the instance already keep two binaries apart.
    static std::atomic<uint64_t>& next() {
        static std::atomic<uint64_t> calls{0};
        return calls;
    }

    void release() {
        if (gpu_ != nullptr && !key_.empty()) {
            gpu_->drop(key_);
        }
        key_.clear();
        buffer_ = Buffer{};
    }

    Gpu* gpu_ = nullptr;
    std::string key_;
    Buffer buffer_{};
};

}   // namespace aofx
