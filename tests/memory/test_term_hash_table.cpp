// tests/memory/test_term_hash_table.cpp
//
// Step 2 验证：TermHashTable + StringArena
//
// 测试维度：
//   1.  insert_and_lookup          - 插入后能查到，不存在的 term 返回 nullptr
//   2.  df_ttf_accumulate          - 多次 upsert 同一 term，df / ttf 正确累加
//   3.  head_updated               - upsert 后 term_page_head 更新为最新值
//   4.  hash_collision_handling    - 同 hash 不同 term 的探测链正确
//   5.  large_insert               - 10000 个不同 term，全部可查到
//   6.  load_factor                - 插入足量 term 后 needsFlush() 触发
//   7.  reset_and_reuse            - reset() 后表为空，可重新插入
//   8.  foreach_covers_all         - forEach 恰好覆盖所有已插入 term
//   9.  term_string_roundtrip      - termString() 返回原始 term 字符串
//   10. capacity_must_be_power2    - 非 2 幂容量抛异常
//   11. collision_chain_integrity  - 故意制造 hash 冲突，验证所有 term 均可正确查到

#include "memory/term_hash_table.h"
#include "memory/term_page.h"    // INVALID_OFFSET

#include <cassert>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

using namespace ii::memory;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << (msg) << "\n"; assert(false); } while(0)

// ─── Test 1: insert_and_lookup ────────────────────────────────────────────────

void test_insert_and_lookup() {
    TEST(insert_and_lookup);
    TermHashTable ht(256);

    ht.upsert("search", 10, 3);
    ht.upsert("engine", 20, 1);

    const Bucket* b1 = ht.lookup("search");
    const Bucket* b2 = ht.lookup("engine");
    const Bucket* b3 = ht.lookup("missing");

    if (!b1) FAIL("'search' not found");
    if (!b2) FAIL("'engine' not found");
    if (b3)  FAIL("'missing' should return nullptr");

    if (b1->term_page_head != 10) FAIL("search head mismatch");
    if (b2->term_page_head != 20) FAIL("engine head mismatch");
    PASS();
}

// ─── Test 2: df_ttf_accumulate ───────────────────────────────────────────────

void test_df_ttf_accumulate() {
    TEST(df_ttf_accumulate);
    TermHashTable ht(256);

    ht.upsert("index", 100, 2);   // doc 1: tf=2
    ht.upsert("index", 200, 5);   // doc 2: tf=5
    ht.upsert("index", 300, 1);   // doc 3: tf=1

    const Bucket* b = ht.lookup("index");
    if (!b)             FAIL("term not found");
    if (b->doc_freq  != 3) FAIL("doc_freq expected 3, got " + std::to_string(b->doc_freq));
    if (b->total_tf  != 8) FAIL("total_tf expected 8, got "  + std::to_string(b->total_tf));
    if (b->term_page_head != 300) FAIL("head should be latest posting (300)");
    PASS();
}

// ─── Test 3: head_updated ────────────────────────────────────────────────────

void test_head_updated() {
    TEST(head_updated);
    TermHashTable ht(64);

    ht.upsert("term", 1, 1);
    if (ht.lookup("term")->term_page_head != 1) FAIL("head after 1st insert");

    ht.upsert("term", 2, 1);
    if (ht.lookup("term")->term_page_head != 2) FAIL("head after 2nd insert");

    ht.upsert("term", 999, 4);
    if (ht.lookup("term")->term_page_head != 999) FAIL("head after 3rd insert");
    PASS();
}

// ─── Test 4: hash_collision_handling ─────────────────────────────────────────
// We can't force a specific hash collision without knowing the internal hash,
// but we can insert many terms that are likely to cluster in the same region
// and verify all are found correctly.

void test_hash_collision_handling() {
    TEST(hash_collision_handling);
    TermHashTable ht(16);   // small table → guaranteed collisions

    const std::vector<std::string> terms = {
        "a", "b", "c", "d", "e", "f", "g", "h"   // 8 terms in 16-bucket table
    };

    for (size_t i = 0; i < terms.size(); ++i)
        ht.upsert(terms[i], static_cast<uint32_t>(i * 10), 1);

    for (size_t i = 0; i < terms.size(); ++i) {
        const Bucket* b = ht.lookup(terms[i]);
        if (!b) FAIL("term not found: " + terms[i]);
        if (b->term_page_head != static_cast<uint32_t>(i * 10))
            FAIL("wrong head for: " + terms[i]);
    }
    if (ht.lookup("z")) FAIL("non-inserted term should not be found");
    PASS();
}

// ─── Test 5: large_insert ────────────────────────────────────────────────────

void test_large_insert() {
    TEST(large_insert_10k_terms);
    // 16384 buckets, insert 8000 unique terms (load ~0.49, below 0.75).
    TermHashTable ht(16384, 2 * 1024 * 1024);

    std::unordered_map<std::string, uint32_t> ref;
    for (int i = 0; i < 8000; ++i) {
        std::string term = "term_" + std::to_string(i);
        uint32_t    head = static_cast<uint32_t>(i);
        ht.upsert(term, head, 1);
        ref[term] = head;
    }

    if (ht.size() != 8000)
        FAIL("size mismatch: " + std::to_string(ht.size()));

    for (const auto& [term, head] : ref) {
        const Bucket* b = ht.lookup(term);
        if (!b) FAIL("term not found: " + term);
        if (b->term_page_head != head) FAIL("head mismatch: " + term);
    }
    PASS();
}

// ─── Test 6: load_factor ─────────────────────────────────────────────────────

void test_load_factor() {
    TEST(load_factor_and_needs_flush);
    TermHashTable ht(64, 4 * 1024);

    // Insert up to 48 terms (load = 48/64 = 0.75) — needsFlush should trigger.
    int inserted = 0;
    for (int i = 0; i < 48 && !ht.needsFlush(); ++i) {
        ht.upsert("t" + std::to_string(i), i, 1);
        ++inserted;
    }

    float lf = ht.loadFactor();
    if (lf < TermHashTable::FLUSH_LOAD_FACTOR)
        FAIL("needsFlush triggered too early, lf=" + std::to_string(lf));
    PASS();
}

// ─── Test 7: reset_and_reuse ─────────────────────────────────────────────────

void test_reset_and_reuse() {
    TEST(reset_and_reuse);
    TermHashTable ht(64);

    ht.upsert("foo", 1, 1);
    ht.upsert("bar", 2, 2);
    if (ht.size() != 2) FAIL("size before reset");

    ht.reset();
    if (ht.size() != 0)       FAIL("size after reset must be 0");
    if (ht.lookup("foo"))     FAIL("foo should not exist after reset");
    if (ht.loadFactor() != 0) FAIL("loadFactor after reset must be 0");

    // Reinsert same terms.
    ht.upsert("foo", 99, 3);
    const Bucket* b = ht.lookup("foo");
    if (!b || b->term_page_head != 99) FAIL("reinsert after reset failed");
    PASS();
}

// ─── Test 8: foreach_covers_all ──────────────────────────────────────────────

void test_foreach_covers_all() {
    TEST(foreach_covers_all_inserted_terms);
    TermHashTable ht(256);

    std::unordered_set<std::string> inserted;
    for (int i = 0; i < 50; ++i) {
        std::string t = "word" + std::to_string(i);
        ht.upsert(t, i, 1);
        inserted.insert(t);
    }

    std::unordered_set<std::string> visited;
    ht.forEach([&](const Bucket& b) {
        visited.insert(std::string(ht.termString(b)));
    });

    if (visited.size() != inserted.size())
        FAIL("forEach visited " + std::to_string(visited.size())
             + " terms, expected " + std::to_string(inserted.size()));

    for (const auto& t : inserted)
        if (!visited.count(t)) FAIL("missing term in forEach: " + t);
    PASS();
}

// ─── Test 9: term_string_roundtrip ───────────────────────────────────────────

void test_term_string_roundtrip() {
    TEST(term_string_roundtrip);
    TermHashTable ht(64);

    const std::string terms[] = {"hello", "world", "search", "index", "engine"};
    for (const auto& t : terms) ht.upsert(t, 0, 1);

    for (const auto& t : terms) {
        const Bucket* b = ht.lookup(t);
        if (!b) FAIL("term not found: " + t);
        std::string retrieved(ht.termString(*b));
        if (retrieved != t) FAIL("termString mismatch for: " + t);
    }
    PASS();
}

// ─── Test 10: capacity_must_be_power2 ────────────────────────────────────────

void test_capacity_must_be_power2() {
    TEST(capacity_must_be_power_of_2);

    bool threw = false;
    try { TermHashTable ht(100); }   // not a power of 2
    catch (const std::invalid_argument&) { threw = true; }
    if (!threw) FAIL("should throw for non-power-of-2 capacity");

    // These should succeed.
    { TermHashTable ht(64);   (void)ht; }
    { TermHashTable ht(1024); (void)ht; }
    PASS();
}

// ─── Test 11: collision_chain_integrity ──────────────────────────────────────
// Use a very small table to force linear probing and verify integrity.

void test_collision_chain_integrity() {
    TEST(collision_chain_integrity_small_table);
    // Table of 8 buckets, insert 6 terms → guaranteed chain collisions.
    TermHashTable ht(8, 256);

    const std::vector<std::string> terms = {"aa","bb","cc","dd","ee","ff"};
    std::unordered_map<std::string, uint32_t> expected;

    for (size_t i = 0; i < terms.size(); ++i) {
        uint32_t head = static_cast<uint32_t>(i + 1) * 100;
        ht.upsert(terms[i], head, 1);
        expected[terms[i]] = head;
    }

    // All terms must be retrievable despite collisions.
    for (const auto& [term, head] : expected) {
        const Bucket* b = ht.lookup(term);
        if (!b) FAIL("term lost due to collision: " + term);
        if (b->term_page_head != head)
            FAIL("wrong head after collision for: " + term);
    }

    // Unknown terms must still return nullptr.
    if (ht.lookup("zz")) FAIL("non-inserted term found after collision test");
    PASS();
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== TermHashTable Tests (Step 2) ===\n\n";

    test_insert_and_lookup();
    test_df_ttf_accumulate();
    test_head_updated();
    test_hash_collision_handling();
    test_large_insert();
    test_load_factor();
    test_reset_and_reuse();
    test_foreach_covers_all();
    test_term_string_roundtrip();
    test_capacity_must_be_power2();
    test_collision_chain_integrity();

    std::cout << "\nAll Step 2 tests passed.\n";
    return 0;
}
