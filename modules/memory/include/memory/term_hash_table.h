#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include "memory/segment_arena.h"

namespace ii::memory {

// ─── Bucket (24 bytes) ──────────────────────────────────────────────────────
//
// One slot in the open-addressing hash table.
// term_hash == 0 indicates an empty slot.
//
struct Bucket {
    uint32_t term_hash;       // FNV-1a hash of term string; 0 = empty
    uint32_t str_offset;      // term string start in StringArena
    uint32_t str_len;         // term string byte length
    uint32_t term_page_head;  // arena offset of newest TermPage (list head)
    uint32_t doc_freq;        // number of distinct documents containing this term
    uint32_t total_tf;        // sum of tf across all documents
    // 6 × 4 = 24 bytes
};
static_assert(sizeof(Bucket) == 24, "Bucket must be 24 bytes");

// ─── TermHashTable ───────────────────────────────────────────────────────────
//
// Open-addressing hash table with linear probing.
// Maps term string → posting-list metadata.
//
// Thread-safety:
//   NOT thread-safe. Thread safety is guaranteed architecturally:
//   each IndexWorker owns a private MemorySegment (and thus a private
//   TermHashTable), so only one thread ever calls upsert() on a given
//   instance during the write phase.
//
//   Concurrent reads (via MemorySegmentReader) are safe ONLY after the
//   owning IndexWorker has stopped writing (i.e., after flush/commit).
//   Simultaneous read + write is a data race and must be prevented at
//   the calling layer (see real_time_search.md §4.2 for the epoch-snapshot
//   approach used during ACTIVE → FLUSHING transition).
//
// Other design constraints:
//   - Bucket array is allocated once at construction (no resize during writes).
//   - If load factor reaches the flush threshold the caller should flush the
//     in-memory segment and call reset() before inserting more terms.
//   - Term strings are stored in a separate StringArena (variable-length).
//
class TermHashTable {
public:
    static constexpr float FLUSH_LOAD_FACTOR = 0.75f;

    // capacity      : number of buckets, must be a power of 2.
    // str_arena_mb  : reserved bytes for term string storage.
    explicit TermHashTable(size_t capacity,
                           size_t str_arena_bytes = 8 * 1024 * 1024);
    ~TermHashTable();

    TermHashTable(const TermHashTable&)            = delete;
    TermHashTable& operator=(const TermHashTable&) = delete;

    // Insert a new posting for `term` (newest TermPage at `new_head`).
    // If term already exists: prepend new_head to its list, add tf.
    // If term is new: create bucket, store string, set head.
    // Returns pointer to the (possibly updated) bucket.
    Bucket* upsert(std::string_view term,
                   uint32_t         new_head,
                   uint32_t         tf);

    // Look up term. Returns nullptr if not found.
    const Bucket* lookup(std::string_view term) const;
    Bucket*       lookup(std::string_view term);

    // Retrieve the term string for a bucket (for iteration / flush).
    std::string_view termString(const Bucket& b) const;

    // Iterate all occupied buckets.
    template<typename Func>
    void forEach(Func&& fn) const {
        for (size_t i = 0; i < capacity_; ++i)
            if (buckets_[i].term_hash != 0)
                fn(buckets_[i]);
    }

    size_t size()       const { return size_; }
    size_t capacity()   const { return capacity_; }
    float  loadFactor() const {
        return capacity_ ? static_cast<float>(size_) / capacity_ : 1.f;
    }
    bool needsFlush() const { return loadFactor() >= FLUSH_LOAD_FACTOR; }

    // Clear all buckets and reset string storage. O(capacity).
    void reset();

    const StringArena& stringArena() const { return *str_arena_; }

private:
    static uint32_t computeHash(std::string_view s);
    size_t          startSlot(uint32_t h) const { return h & (capacity_ - 1); }

    // Find slot for term with the given hash; returns capacity_ if not found.
    size_t findSlot(std::string_view term, uint32_t h) const;

    Bucket*                     buckets_;
    size_t                      capacity_;
    size_t                      size_;
    std::unique_ptr<StringArena> str_arena_;
};

} // namespace ii::memory
