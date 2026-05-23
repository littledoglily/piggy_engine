#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace ii::memory {

static constexpr uint32_t INVALID_OFFSET = UINT32_MAX;

// Bump allocator: all allocations come from a single contiguous block.
// Offsets (uint32_t) are used instead of pointers so page structs stay
// 32-bit and the entire arena can be reset in O(1) after a flush.
class SegmentArena {
public:
    explicit SegmentArena(size_t capacity_bytes);
    ~SegmentArena();

    SegmentArena(const SegmentArena&)            = delete;
    SegmentArena& operator=(const SegmentArena&) = delete;

    // Allocate `size` bytes aligned to `align`. Returns arena-relative offset.
    // Throws std::bad_alloc when capacity is exhausted.
    uint32_t alloc(size_t size, size_t align = 4);

    // Convert arena offset → typed pointer. UB if offset is INVALID_OFFSET.
    template<typename T>
    T* at(uint32_t offset) {
        return reinterpret_cast<T*>(base_ + offset);
    }
    template<typename T>
    const T* at(uint32_t offset) const {
        return reinterpret_cast<const T*>(base_ + offset);
    }

    // Reset bump pointer to zero — O(1), memory is immediately reusable.
    void reset();

    size_t used()     const { return offset_; }
    size_t capacity() const { return capacity_; }
    bool   full(size_t needed = 1) const { return offset_ + needed > capacity_; }

private:
    uint8_t* base_;
    size_t   capacity_;
    size_t   offset_;
};

// Append-only string storage: variable-length term strings live here.
// Separate from SegmentArena so fixed-size page allocations are not
// fragmented by variable-length strings.
class StringArena {
public:
    explicit StringArena(size_t capacity_bytes);
    ~StringArena();

    StringArena(const StringArena&)            = delete;
    StringArena& operator=(const StringArena&) = delete;

    // Store a string and return its offset. The string is NOT null-terminated.
    // Throws std::bad_alloc when capacity is exhausted.
    uint32_t store(std::string_view s);

    // Retrieve previously stored string by offset + length.
    std::string_view get(uint32_t offset, uint32_t len) const;

    void   reset();
    size_t used()     const { return offset_; }
    size_t capacity() const { return capacity_; }

private:
    char*  base_;
    size_t capacity_;
    size_t offset_;
};

} // namespace ii::memory
