#pragma once
#include <cstdint>
#include <cstddef>
#include "memory/segment_arena.h"

namespace ii::memory {

// ─── Page size constants ────────────────────────────────────────────────────

static constexpr size_t TERM_PAGE_SIZE   = 24;
static constexpr size_t POS_PAGE_SIZE    = 32;
static constexpr size_t STORED_PAGE_SIZE = 4096;

// tf threshold below which positions are stored inline in TermPage.
// tf <= INLINE_POS_LIMIT: no PosPage is allocated.
static constexpr int INLINE_POS_LIMIT = 3;

// Maximum positions stored in a single PosPage before chaining.
static constexpr int POS_PER_PAGE = 7;

// Usable bytes for stored-field content in a StoredPage (header = 12B).
static constexpr size_t STORED_PAGE_DATA_SIZE = STORED_PAGE_SIZE - 12;

// ─── TermPage (24 bytes) ────────────────────────────────────────────────────
//
// One posting entry: records that `doc_id` contains this term `tf` times.
// Forms a singly-linked list (newest doc at head) per term.
//
// Position storage:
//   tf <= INLINE_POS_LIMIT  →  positions[] stored inline (no PosPage alloc)
//   tf >  INLINE_POS_LIMIT  →  pos_head points to first PosPage
//
struct TermPage {
    uint32_t doc_id;   // document ID
    uint32_t tf;       // term frequency in this document
    uint32_t next;     // next TermPage offset in posting list (INVALID_OFFSET = end)
    union {
        uint32_t pos_head;       // offset of first PosPage (tf > INLINE_POS_LIMIT)
        uint32_t inline_pos[3];  // up to 3 positions stored directly (tf <= 3)
    };
    // 4 + 4 + 4 + 12 = 24 bytes
};
static_assert(sizeof(TermPage) == TERM_PAGE_SIZE, "TermPage must be 24 bytes");

// ─── PosPage (32 bytes) ─────────────────────────────────────────────────────
//
// One chunk of a position list. Chains via `next` when tf > POS_PER_PAGE.
//
struct PosPage {
    uint32_t positions[POS_PER_PAGE];  // position offsets within the document
    uint32_t next;                     // next PosPage offset (INVALID_OFFSET = end)
    // 7*4 + 4 = 32 bytes
};
static_assert(sizeof(PosPage) == POS_PAGE_SIZE, "PosPage must be 32 bytes");

// ─── StoredPage (4096 bytes) ─────────────────────────────────────────────────
//
// One chunk of stored-field content (equivalent to on-disk .fdt).
// Large documents span multiple StoredPages via `next`.
//
struct StoredPageHeader {
    uint32_t doc_id;    // owning document ID
    uint32_t data_len;  // bytes of content in this page (≤ STORED_PAGE_DATA_SIZE)
    uint32_t next;      // next StoredPage offset (INVALID_OFFSET = end)
    // Total header: 12 bytes; data immediately follows in the 4KB page.
};
static_assert(sizeof(StoredPageHeader) == 12, "StoredPageHeader must be 12 bytes");

// ─── Allocation helpers ──────────────────────────────────────────────────────

inline uint32_t allocTermPage(SegmentArena& arena) {
    return arena.alloc(TERM_PAGE_SIZE, 4);
}

inline uint32_t allocPosPage(SegmentArena& arena) {
    return arena.alloc(POS_PAGE_SIZE, 4);
}

inline uint32_t allocStoredPage(SegmentArena& arena) {
    return arena.alloc(STORED_PAGE_SIZE, 4);
}

} // namespace ii::memory
