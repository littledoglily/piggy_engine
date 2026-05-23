#include "memory/term_hash_table.h"
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace ii::memory {

// ─── Hash function (FNV-1a 32-bit) ──────────────────────────────────────────
// Good distribution, no external dependencies.
// Returns a value in [1, UINT32_MAX] — 0 is reserved for "empty slot".

uint32_t TermHashTable::computeHash(std::string_view s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h == 0 ? 1u : h;
}

// ─── TermHashTable ────────────────────────────────────────────────────────────

TermHashTable::TermHashTable(size_t capacity, size_t str_arena_bytes)
    : buckets_(nullptr)
    , capacity_(capacity)
    , size_(0)
    , str_arena_(std::make_unique<StringArena>(str_arena_bytes))
{
    if (capacity == 0 || (capacity & (capacity - 1)) != 0)
        throw std::invalid_argument("TermHashTable: capacity must be a power of 2");

    buckets_ = static_cast<Bucket*>(std::calloc(capacity, sizeof(Bucket)));
    if (!buckets_) throw std::bad_alloc();
    // calloc zeroes memory: term_hash == 0 marks every slot as empty.
}

TermHashTable::~TermHashTable() {
    std::free(buckets_);
}

// Find the slot index for `term` with pre-computed hash `h`.
// Returns the index of the matching occupied slot, or the first empty slot
// encountered (which is where the term should be inserted).
size_t TermHashTable::findSlot(std::string_view term, uint32_t h) const {
    size_t idx = startSlot(h);
    for (size_t probe = 0; probe < capacity_; ++probe) {
        const Bucket& b = buckets_[idx];
        if (b.term_hash == 0) return idx;          // empty: insertion point
        if (b.term_hash == h &&
            b.str_len   == static_cast<uint32_t>(term.size()) &&
            str_arena_->get(b.str_offset, b.str_len) == term)
            return idx;                            // found existing term
        idx = (idx + 1) & (capacity_ - 1);        // linear probe
    }
    // Full table — caller should have triggered flush before reaching this.
    throw std::overflow_error("TermHashTable: table is full");
}

Bucket* TermHashTable::upsert(std::string_view term,
                               uint32_t         new_head,
                               uint32_t         tf)
{
    uint32_t h   = computeHash(term);
    size_t   idx = findSlot(term, h);
    Bucket&  b   = buckets_[idx];

    if (b.term_hash == 0) {
        // New term: store string and initialise bucket.
        b.str_offset     = str_arena_->store(term);
        b.str_len        = static_cast<uint32_t>(term.size());
        b.term_hash      = h;
        b.term_page_head = new_head;
        b.doc_freq       = 1;
        b.total_tf       = tf;
        ++size_;
    } else {
        // Existing term: the caller has already set new_head.next = old head
        // (linking the new TermPage into the list). We just update metadata.
        b.term_page_head = new_head;
        b.doc_freq      += 1;
        b.total_tf      += tf;
    }
    return &b;
}

const Bucket* TermHashTable::lookup(std::string_view term) const {
    uint32_t h   = computeHash(term);
    size_t   idx = findSlot(term, h);
    const Bucket& b = buckets_[idx];
    return b.term_hash == 0 ? nullptr : &b;
}

Bucket* TermHashTable::lookup(std::string_view term) {
    uint32_t h   = computeHash(term);
    size_t   idx = findSlot(term, h);
    Bucket& b    = buckets_[idx];
    return b.term_hash == 0 ? nullptr : &b;
}

std::string_view TermHashTable::termString(const Bucket& b) const {
    return str_arena_->get(b.str_offset, b.str_len);
}

void TermHashTable::reset() {
    std::memset(buckets_, 0, capacity_ * sizeof(Bucket));
    size_ = 0;
    str_arena_->reset();
}

} // namespace ii::memory
