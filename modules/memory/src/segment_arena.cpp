#include "memory/segment_arena.h"
#include <cstdlib>
#include <cstring>
#include <new>

namespace ii::memory {

// ─── SegmentArena ────────────────────────────────────────────────────────────

SegmentArena::SegmentArena(size_t capacity_bytes)
    : base_(static_cast<uint8_t*>(std::malloc(capacity_bytes)))
    , capacity_(capacity_bytes)
    , offset_(0)
{
    if (!base_) throw std::bad_alloc();
}

SegmentArena::~SegmentArena() {
    std::free(base_);
}

uint32_t SegmentArena::alloc(size_t size, size_t align) {
    // Round up current offset to the required alignment.
    size_t aligned = (offset_ + align - 1) & ~(align - 1);
    if (aligned + size > capacity_)
        throw std::bad_alloc();
    offset_ = aligned + size;
    return static_cast<uint32_t>(aligned);
}

void SegmentArena::reset() {
    offset_ = 0;
}

// ─── StringArena ─────────────────────────────────────────────────────────────

StringArena::StringArena(size_t capacity_bytes)
    : base_(static_cast<char*>(std::malloc(capacity_bytes)))
    , capacity_(capacity_bytes)
    , offset_(0)
{
    if (!base_) throw std::bad_alloc();
}

StringArena::~StringArena() {
    std::free(base_);
}

uint32_t StringArena::store(std::string_view s) {
    if (offset_ + s.size() > capacity_)
        throw std::bad_alloc();
    std::memcpy(base_ + offset_, s.data(), s.size());
    uint32_t off = static_cast<uint32_t>(offset_);
    offset_ += s.size();
    return off;
}

std::string_view StringArena::get(uint32_t offset, uint32_t len) const {
    return {base_ + offset, len};
}

void StringArena::reset() {
    offset_ = 0;
}

} // namespace ii::memory
