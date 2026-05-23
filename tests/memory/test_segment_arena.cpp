// tests/memory/test_segment_arena.cpp
//
// Step 1 验证：SegmentArena, StringArena, 三种 Page 结构
//
// 测试维度：
//   1.  basic_alloc           - 基本分配，offset 递增，无重叠
//   2.  alignment             - 4/8/16 字节对齐正确
//   3.  ptr_roundtrip         - at<T>() 取回写入的值
//   4.  used_tracking         - used() 随分配增长
//   5.  exhaustion_throws     - 超容量抛 bad_alloc
//   6.  reset_and_reuse       - reset() 后 offset 归零，内存可复用
//   7.  term_page_layout      - TermPage 大小 / 成员 / union 正确
//   8.  pos_page_layout       - PosPage 大小 / chain 读写
//   9.  stored_page_layout    - StoredPageHeader 大小 / data 区域
//   10. alloc_helpers         - allocTermPage / allocPosPage / allocStoredPage
//   11. string_arena_store    - StringArena store / get roundtrip
//   12. string_arena_reset    - StringArena reset 后可复用
//   13. string_arena_exhaust  - StringArena 超容量抛 bad_alloc
//   14. multi_page_chain      - 用 PosPage 链模拟 tf=20 的 position list

#include "memory/segment_arena.h"
#include "memory/term_page.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace ii::memory;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << (msg) << "\n"; assert(false); } while(0)

// ─── helpers ─────────────────────────────────────────────────────────────────

static bool isPowerOf2(size_t n) { return n && !(n & (n - 1)); }

// ─── Test 1: basic_alloc ─────────────────────────────────────────────────────

void test_basic_alloc() {
    TEST(basic_alloc);
    SegmentArena arena(1024);

    uint32_t a = arena.alloc(4);
    uint32_t b = arena.alloc(4);
    uint32_t c = arena.alloc(8);

    if (a != 0)  FAIL("first alloc should start at 0");
    if (b != 4)  FAIL("second alloc should be at 4");
    if (c != 8)  FAIL("third alloc should be at 8");
    if (arena.used() != 16) FAIL("used should be 16");
    PASS();
}

// ─── Test 2: alignment ───────────────────────────────────────────────────────

void test_alignment() {
    TEST(alignment);
    SegmentArena arena(256);

    arena.alloc(1);              // offset 0, used=1
    uint32_t a4  = arena.alloc(4, 4);   // must be aligned to 4 → offset 4
    uint32_t a8  = arena.alloc(1);      // offset 8
    uint32_t a8b = arena.alloc(8, 8);   // must be aligned to 8 → offset 16
    uint32_t a16 = arena.alloc(1);      // offset 24
    uint32_t a16b= arena.alloc(4, 16);  // must be aligned to 16 → offset 32

    if (a4   % 4  != 0) FAIL("align-4 failed");
    if (a8b  % 8  != 0) FAIL("align-8 failed");
    if (a16b % 16 != 0) FAIL("align-16 failed");
    (void)a8; (void)a16;
    PASS();
}

// ─── Test 3: ptr_roundtrip ───────────────────────────────────────────────────

void test_ptr_roundtrip() {
    TEST(ptr_roundtrip);
    SegmentArena arena(256);

    uint32_t off = arena.alloc(sizeof(uint64_t), 8);
    *arena.at<uint64_t>(off) = 0xDEADBEEFCAFEBABEull;

    if (*arena.at<uint64_t>(off) != 0xDEADBEEFCAFEBABEull)
        FAIL("value mismatch after write");
    PASS();
}

// ─── Test 4: used_tracking ───────────────────────────────────────────────────

void test_used_tracking() {
    TEST(used_tracking);
    SegmentArena arena(1024);
    if (arena.used() != 0) FAIL("initial used must be 0");

    arena.alloc(13, 1);
    size_t u1 = arena.used();
    arena.alloc(7, 1);
    size_t u2 = arena.used();

    if (u1 != 13) FAIL("used after first alloc");
    if (u2 != 20) FAIL("used after second alloc");
    PASS();
}

// ─── Test 5: exhaustion_throws ───────────────────────────────────────────────

void test_exhaustion_throws() {
    TEST(exhaustion_throws);
    SegmentArena arena(16);
    arena.alloc(16, 1);

    bool threw = false;
    try { arena.alloc(1, 1); }
    catch (const std::bad_alloc&) { threw = true; }
    if (!threw) FAIL("should throw bad_alloc when exhausted");
    PASS();
}

// ─── Test 6: reset_and_reuse ─────────────────────────────────────────────────

void test_reset_and_reuse() {
    TEST(reset_and_reuse);
    SegmentArena arena(64);

    arena.alloc(32, 1);
    if (arena.used() != 32) FAIL("used before reset");

    arena.reset();
    if (arena.used() != 0) FAIL("used after reset must be 0");

    // Should be able to allocate the full capacity again.
    uint32_t off = arena.alloc(64, 1);
    if (off != 0) FAIL("first offset after reset must be 0");
    PASS();
}

// ─── Test 7: term_page_layout ────────────────────────────────────────────────

void test_term_page_layout() {
    TEST(term_page_layout);

    static_assert(sizeof(TermPage) == TERM_PAGE_SIZE,
                  "TermPage size mismatch");

    SegmentArena arena(256);
    uint32_t off = allocTermPage(arena);
    TermPage* tp = arena.at<TermPage>(off);
    std::memset(tp, 0, sizeof(TermPage));

    // Write via inline_pos union (tf <= INLINE_POS_LIMIT).
    tp->doc_id = 42;
    tp->tf     = 3;
    tp->next   = INVALID_OFFSET;
    tp->inline_pos[0] = 5;
    tp->inline_pos[1] = 12;
    tp->inline_pos[2] = 99;

    if (arena.at<TermPage>(off)->inline_pos[2] != 99)
        FAIL("inline_pos write-back failed");

    // Write via pos_head union (tf > INLINE_POS_LIMIT).
    tp->tf       = 10;
    tp->pos_head = 0xABCD;
    if (arena.at<TermPage>(off)->pos_head != 0xABCD)
        FAIL("pos_head write-back failed");

    PASS();
}

// ─── Test 8: pos_page_layout ─────────────────────────────────────────────────

void test_pos_page_layout() {
    TEST(pos_page_layout);

    static_assert(sizeof(PosPage) == POS_PAGE_SIZE, "PosPage size mismatch");

    SegmentArena arena(1024);

    // Build a two-page chain for positions [0..13].
    uint32_t p1_off = allocPosPage(arena);
    uint32_t p2_off = allocPosPage(arena);

    PosPage* p1 = arena.at<PosPage>(p1_off);
    PosPage* p2 = arena.at<PosPage>(p2_off);

    for (int i = 0; i < POS_PER_PAGE; ++i) p1->positions[i] = i;
    p1->next = p2_off;

    for (int i = 0; i < 7; ++i) p2->positions[i] = 100 + i;
    p2->next = INVALID_OFFSET;

    // Traverse and verify.
    uint32_t cur = p1_off;
    int idx = 0;
    while (cur != INVALID_OFFSET) {
        PosPage* p = arena.at<PosPage>(cur);
        for (int i = 0; i < POS_PER_PAGE; ++i) {
            uint32_t expected = (idx == 0) ? (uint32_t)i : (uint32_t)(100 + i);
            if (p->positions[i] != expected)
                FAIL("position value mismatch in chain");
        }
        ++idx;
        cur = p->next;
    }
    if (idx != 2) FAIL("expected 2 pages in chain");
    PASS();
}

// ─── Test 9: stored_page_layout ──────────────────────────────────────────────

void test_stored_page_layout() {
    TEST(stored_page_layout);

    static_assert(sizeof(StoredPageHeader) == 12, "StoredPageHeader size mismatch");
    static_assert(STORED_PAGE_SIZE == 4096,       "STORED_PAGE_SIZE must be 4096");
    static_assert(STORED_PAGE_DATA_SIZE == 4084,  "STORED_PAGE_DATA_SIZE must be 4084");

    SegmentArena arena(STORED_PAGE_SIZE * 2);
    uint32_t off = allocStoredPage(arena);

    StoredPageHeader* hdr = arena.at<StoredPageHeader>(off);
    hdr->doc_id   = 7;
    hdr->data_len = 100;
    hdr->next     = INVALID_OFFSET;

    // Data area immediately follows the header.
    uint8_t* data = reinterpret_cast<uint8_t*>(hdr) + sizeof(StoredPageHeader);
    std::memset(data, 0xAA, 100);

    if (data[0] != 0xAA || data[99] != 0xAA)
        FAIL("stored page data area not writable");
    if (hdr->data_len != 100)
        FAIL("header field clobbered by data write");

    PASS();
}

// ─── Test 10: alloc_helpers ──────────────────────────────────────────────────

void test_alloc_helpers() {
    TEST(alloc_helpers);
    // All three page types must be naturally aligned (to their own size).
    SegmentArena arena(STORED_PAGE_SIZE * 4);

    uint32_t tp = allocTermPage(arena);
    uint32_t pp = allocPosPage(arena);
    uint32_t sp = allocStoredPage(arena);

    if (tp % 4 != 0) FAIL("TermPage not 4-byte aligned");
    if (pp % 4 != 0) FAIL("PosPage not 4-byte aligned");
    if (sp % 4 != 0) FAIL("StoredPage not 4-byte aligned");

    // No overlap: each region must not touch the others.
    auto inRange = [](uint32_t off, uint32_t base, size_t sz) {
        return off >= base && off < base + sz;
    };
    if (inRange(pp, tp, TERM_PAGE_SIZE))   FAIL("PosPage overlaps TermPage");
    if (inRange(sp, pp, POS_PAGE_SIZE))    FAIL("StoredPage overlaps PosPage");
    PASS();
}

// ─── Test 11: string_arena_store ─────────────────────────────────────────────

void test_string_arena_store() {
    TEST(string_arena_store);
    StringArena sa(1024);

    uint32_t off1 = sa.store("hello");
    uint32_t off2 = sa.store("world");
    uint32_t off3 = sa.store("");        // empty string

    if (sa.get(off1, 5) != "hello") FAIL("string 1 mismatch");
    if (sa.get(off2, 5) != "world") FAIL("string 2 mismatch");
    if (sa.get(off3, 0) != "")      FAIL("empty string mismatch");
    if (off2 != off1 + 5)           FAIL("offsets not contiguous");
    PASS();
}

// ─── Test 12: string_arena_reset ─────────────────────────────────────────────

void test_string_arena_reset() {
    TEST(string_arena_reset);
    StringArena sa(64);

    sa.store("some term");
    if (sa.used() == 0) FAIL("used should be non-zero");

    sa.reset();
    if (sa.used() != 0) FAIL("used after reset must be 0");

    uint32_t off = sa.store("reused");
    if (off != 0)             FAIL("first offset after reset must be 0");
    if (sa.get(off, 6) != "reused") FAIL("string after reset");
    PASS();
}

// ─── Test 13: string_arena_exhaust ───────────────────────────────────────────

void test_string_arena_exhaust() {
    TEST(string_arena_exhaust);
    StringArena sa(4);
    sa.store("hi");   // 2 bytes, ok
    sa.store("ok");   // 2 bytes, ok

    bool threw = false;
    try { sa.store("x"); }
    catch (const std::bad_alloc&) { threw = true; }
    if (!threw) FAIL("should throw bad_alloc when string arena is full");
    PASS();
}

// ─── Test 14: multi_page_chain ───────────────────────────────────────────────
// Simulate storing 20 positions via linked PosPage chain.

void test_multi_page_chain() {
    TEST(multi_page_chain_pos20);
    SegmentArena arena(64 * 1024);

    constexpr int TF = 20;
    uint32_t head = INVALID_OFFSET;

    // Allocate pages and fill positions 0..19 in chain order.
    uint32_t written = 0;
    uint32_t cur_off = INVALID_OFFSET;

    for (int i = 0; i < TF; ) {
        uint32_t poff = allocPosPage(arena);
        PosPage* pp   = arena.at<PosPage>(poff);
        pp->next      = INVALID_OFFSET;

        int count = 0;
        while (i < TF && count < POS_PER_PAGE) {
            pp->positions[count++] = static_cast<uint32_t>(i++);
        }
        // Zero remaining slots.
        for (int j = count; j < POS_PER_PAGE; ++j) pp->positions[j] = 0;

        if (head == INVALID_OFFSET) {
            head = poff;
            cur_off = poff;
        } else {
            arena.at<PosPage>(cur_off)->next = poff;
            cur_off = poff;
        }
        written += count;
    }

    if (written != TF) FAIL("wrong total positions written");

    // Traverse and verify values 0..TF-1.
    uint32_t expected = 0;
    uint32_t page_cur = head;
    while (page_cur != INVALID_OFFSET) {
        PosPage* p = arena.at<PosPage>(page_cur);
        int count = (expected + POS_PER_PAGE <= TF) ? POS_PER_PAGE
                                                     : (TF - expected);
        for (int i = 0; i < count; ++i) {
            if (p->positions[i] != expected)
                FAIL("position mismatch at " + std::to_string(expected));
            ++expected;
        }
        page_cur = p->next;
    }
    if (expected != TF) FAIL("did not traverse all positions");
    PASS();
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== SegmentArena & Page Layout Tests (Step 1) ===\n\n";

    test_basic_alloc();
    test_alignment();
    test_ptr_roundtrip();
    test_used_tracking();
    test_exhaustion_throws();
    test_reset_and_reuse();
    test_term_page_layout();
    test_pos_page_layout();
    test_stored_page_layout();
    test_alloc_helpers();
    test_string_arena_store();
    test_string_arena_reset();
    test_string_arena_exhaust();
    test_multi_page_chain();

    std::cout << "\nAll Step 1 tests passed.\n";
    return 0;
}
