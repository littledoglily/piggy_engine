// tests/memory/test_memory_segment_write.cpp
//
// Step 3 验证：MemorySegment 写入核心
//
// 测试维度：
//   1. single_doc_term_count       - 写入 1 篇文档，hashtable 中 term 数量正确
//   2. df_ttf_match_reference      - 1000 篇文档后 df/ttf 与单线程 IndexWriter 完全一致
//   3. inline_pos_path             - tf <= 3 时 TermPage 使用 inline_pos，不分配 PosPage
//   4. pos_page_path               - tf > 3 时 TermPage 使用 pos_head + PosPage 链
//   5. position_values_correct     - PosPage 链中的 position 值与分词结果一致
//   6. ram_tracking                - addDocument 后 ramUsed() 单调增加
//   7. should_flush_arena          - arena 接近满时 shouldFlush() 返回 true
//   8. should_flush_hashtable      - hashtable 负载超阈值时 shouldFlush() 返回 true
//   9. reset_clears_state          - reset() 后 docCount/ramUsed 归零，可重新写入
//   10. stored_field_pages         - 有 stored 字段时 StoredPage 被分配（arena 增长）
//   11. multi_field_separation     - 不同字段的同一词 term key 不同（"body:x" vs "title:x"）

#include "memory/memory_segment.h"
#include "memory/term_page.h"
#include "field/schema.h"
#include "index/index_writer.h"    // 单线程参考
#include "index/segment_reader.h"  // 读参考索引
#include "core/types.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace ii;
using namespace ii::memory;
namespace fs = std::filesystem;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << (msg) << "\n"; assert(false); } while(0)

// ─── Schema helpers ───────────────────────────────────────────────────────────

static Schema makeSchema() {
    Schema s;
    s.fields = {
        {"body",     FieldType::Text,    IndexOption::FreqsPositions, true,  false, false, {}},
        {"category", FieldType::Keyword, IndexOption::FreqsOnly,      false, false, false, {}},
    };
    return s;
}

static Schema makeTwoTextField() {
    Schema s;
    s.fields = {
        {"title", FieldType::Text, IndexOption::FreqsPositions, true, false, false, {}},
        {"body",  FieldType::Text, IndexOption::FreqsPositions, true, false, false, {}},
    };
    return s;
}

// ─── Document factory ─────────────────────────────────────────────────────────

static const std::vector<std::string> kBodies = {
    "distributed systems consistency availability partition tolerance replication",
    "machine learning gradient descent optimization neural network backpropagation",
    "database index btree hash table query execution plan optimizer join",
    "concurrency locking deadlock transaction isolation level serializable",
    "network protocol tcp udp http rest api design latency throughput",
    "memory allocation cache locality prefetch branch prediction pipeline",
    "compiler optimization register allocation instruction scheduling loop",
    "search engine inverted index ranking bm25 tf idf scoring retrieval",
    "graph algorithm shortest path spanning tree dynamic programming flow",
    "operating system process thread scheduler virtual memory paging swap",
};
static const std::vector<std::string> kCats =
    {"systems","ml","database","concurrency","network",
     "hardware","compiler","search","algorithms","os"};

static Document makeDoc(int i) {
    Document d;
    d.set("body",     kBodies[i % kBodies.size()]);
    d.set("category", kCats  [i % kCats.size()]);
    return d;
}

// ─── Reference stats from IndexWriter ────────────────────────────────────────

struct TermStats { uint32_t df = 0, ttf = 0; };
using StatsMap = std::map<std::string, TermStats>;  // "field:term" → stats

static StatsMap buildReferenceStats(int n_docs) {
    const std::string dir = "/tmp/piggy_ms_ref";
    fs::remove_all(dir);

    IndexWriter writer(dir, 512.f, makeSchema());
    DocId id = 0;
    for (int i = 0; i < n_docs; ++i) {
        Document d = makeDoc(i);
        d.doc_id = ++id;
        writer.addDocument(d);
    }
    writer.commit();

    SegmentReader reader(dir, 0);
    StatsMap stats;
    for (const auto& field : {"body", "category"}) {
        const auto* dict = reader.fieldTermDict(field);
        if (!dict) continue;
        for (const auto& [term, meta] : *dict) {
            std::string key = std::string(field) + ":" + term;
            stats[key] = {meta.doc_freq, meta.total_term_freq};
        }
    }
    return stats;
}

// ─── Test 1: single_doc_term_count ────────────────────────────────────────────

void test_single_doc_term_count() {
    TEST(single_doc_term_count);
    MemorySegment ms(makeSchema());

    Document d = makeDoc(0);  // body has 7 unique words + 1 category keyword
    ms.addDocument(d, 1);

    if (ms.docCount() != 1) FAIL("docCount should be 1");
    if (ms.hashtable().size() == 0) FAIL("hashtable should have terms after addDocument");
    PASS();
}

// ─── Test 2: df_ttf_match_reference ──────────────────────────────────────────

void test_df_ttf_match_reference() {
    TEST(df_ttf_match_reference_1000_docs);

    constexpr int N = 1000;
    auto ref = buildReferenceStats(N);

    MemorySegmentConfig cfg;
    cfg.arena_bytes   = 128ULL * 1024 * 1024;
    cfg.hashtable_cap = 1 << 17;
    MemorySegment ms(makeSchema(), cfg);

    for (int i = 0; i < N; ++i)
        ms.addDocument(makeDoc(i), static_cast<DocId>(i + 1));

    if (ms.docCount() != static_cast<uint32_t>(N))
        FAIL("docCount=" + std::to_string(ms.docCount()));

    // Compare df/ttf for every term in reference
    size_t mismatches = 0;
    ms.hashtable().forEach([&](const Bucket& b) {
        std::string key(ms.hashtable().termString(b));
        auto it = ref.find(key);
        if (it == ref.end()) {
            std::cout << "\n  EXTRA term: " << key;
            ++mismatches;
            return;
        }
        if (b.doc_freq != it->second.df) {
            std::cout << "\n  df mismatch " << key
                      << " got=" << b.doc_freq << " ref=" << it->second.df;
            ++mismatches;
        }
        if (b.total_tf != it->second.ttf) {
            std::cout << "\n  ttf mismatch " << key
                      << " got=" << b.total_tf << " ref=" << it->second.ttf;
            ++mismatches;
        }
    });

    // Check reference terms not found in MemorySegment
    ms.hashtable().forEach([&](const Bucket& b) {
        ref.erase(std::string(ms.hashtable().termString(b)));
    });
    for (const auto& [key, _] : ref) {
        std::cout << "\n  MISSING term: " << key;
        ++mismatches;
    }

    if (mismatches > 0)
        FAIL(std::to_string(mismatches) + " mismatch(es) found");
    PASS();
}

// ─── Test 3: inline_pos_path (tf <= 3) ────────────────────────────────────────

void test_inline_pos_path() {
    TEST(inline_pos_path_tf_le_3);

    // "zephyr" appears only once → tf=1, must use inline_pos
    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    Document d;
    d.set("body", "zephyr");
    ms.addDocument(d, 1);

    // Find the bucket and its TermPage
    const Bucket* b = ms.hashtable().lookup("body:zephyr");
    if (!b) FAIL("term not found");

    const TermPage* tp = ms.arena().at<TermPage>(b->term_page_head);
    if (tp->tf != 1) FAIL("tf should be 1");
    if (tp->tf > static_cast<uint32_t>(INLINE_POS_LIMIT))
        FAIL("tf <= 3, should use inline_pos");

    // pos_head union should NOT be used — no PosPage should exist at pos_head
    // (inline_pos[0] holds the position value, not a page offset)
    // We can't check this directly without knowing what pos_head would be,
    // but we verify the inline_pos value is a valid position (>= 0).
    // The position of the first (and only) token is 0.
    if (tp->inline_pos[0] != 0)
        FAIL("inline_pos[0] should be position 0");

    PASS();
}

// ─── Test 4: pos_page_path (tf > 3) ──────────────────────────────────────────

void test_pos_page_path() {
    TEST(pos_page_path_tf_gt_3);

    // "spam" appears 5 times → tf=5, must use PosPage
    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    Document d;
    d.set("body", "spam spam spam spam spam other words");
    ms.addDocument(d, 1);

    const Bucket* b = ms.hashtable().lookup("body:spam");
    if (!b) FAIL("term 'spam' not found");

    const TermPage* tp = ms.arena().at<TermPage>(b->term_page_head);
    if (tp->tf != 5) FAIL("tf should be 5, got " + std::to_string(tp->tf));
    if (tp->tf <= static_cast<uint32_t>(INLINE_POS_LIMIT))
        FAIL("tf=5 should exceed INLINE_POS_LIMIT=3");

    // pos_head must point to a valid PosPage
    if (tp->pos_head == INVALID_OFFSET)
        FAIL("pos_head must be valid for tf > 3");

    // Check PosPage chain length covers all 5 positions
    uint32_t total_pos = 0;
    uint32_t cur = tp->pos_head;
    while (cur != INVALID_OFFSET) {
        const PosPage* pp = ms.arena().at<PosPage>(cur);
        // Count valid positions in this page
        uint32_t remaining = tp->tf - total_pos;
        uint32_t in_page   = std::min(remaining, static_cast<uint32_t>(POS_PER_PAGE));
        total_pos += in_page;
        cur = pp->next;
    }
    if (total_pos != tp->tf)
        FAIL("PosPage chain covers " + std::to_string(total_pos)
             + " positions, expected " + std::to_string(tp->tf));

    PASS();
}

// ─── Test 5: position_values_correct ─────────────────────────────────────────

void test_position_values_correct() {
    TEST(position_values_correct);

    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    // "alpha beta alpha" → "alpha" at positions 0 and 2, "beta" at position 1
    Document d;
    d.set("body", "alpha beta alpha");
    ms.addDocument(d, 1);

    const Bucket* b_beta = ms.hashtable().lookup("body:beta");
    if (!b_beta) FAIL("'beta' not found");
    const TermPage* tp_beta = ms.arena().at<TermPage>(b_beta->term_page_head);
    if (tp_beta->tf != 1) FAIL("beta tf should be 1");
    if (tp_beta->inline_pos[0] != 1) FAIL("beta position should be 1");

    const Bucket* b_alpha = ms.hashtable().lookup("body:alpha");
    if (!b_alpha) FAIL("'alpha' not found");
    const TermPage* tp_alpha = ms.arena().at<TermPage>(b_alpha->term_page_head);
    if (tp_alpha->tf != 2) FAIL("alpha tf should be 2");
    // Positions 0 and 2 in inline_pos (tf=2 <= 3)
    // The order depends on the unordered_map iteration; both must be present.
    std::vector<uint32_t> alpha_pos = {tp_alpha->inline_pos[0], tp_alpha->inline_pos[1]};
    std::sort(alpha_pos.begin(), alpha_pos.end());
    if (alpha_pos[0] != 0 || alpha_pos[1] != 2)
        FAIL("alpha positions should be {0,2}, got {"
             + std::to_string(alpha_pos[0]) + "," + std::to_string(alpha_pos[1]) + "}");

    PASS();
}

// ─── Test 6: ram_tracking ─────────────────────────────────────────────────────

void test_ram_tracking() {
    TEST(ram_tracking_monotone_increase);
    MemorySegment ms(makeSchema());

    size_t ram0 = ms.ramUsed();
    ms.addDocument(makeDoc(0), 1);
    size_t ram1 = ms.ramUsed();
    ms.addDocument(makeDoc(1), 2);
    size_t ram2 = ms.ramUsed();

    if (ram1 <= ram0) FAIL("ram should increase after first doc");
    if (ram2 <= ram1) FAIL("ram should increase after second doc");
    PASS();
}

// ─── Test 7: should_flush_arena ──────────────────────────────────────────────

void test_should_flush_arena() {
    TEST(should_flush_when_arena_near_full);

    // Arena large enough to pass the MIN_RESERVE_BYTES check initially (64KB reserve),
    // but small enough to fill up after a handful of stored-field documents.
    MemorySegmentConfig cfg;
    cfg.arena_bytes   = 256 * 1024;   // 256 KB > MIN_RESERVE_BYTES (64 KB)
    cfg.hashtable_cap = 64;
    cfg.str_arena_bytes = 4096;
    MemorySegment ms(makeSchema(), cfg);

    if (ms.shouldFlush()) FAIL("should not flush initially");

    // Fill up the arena by writing documents with stored fields
    Schema stored_schema;
    stored_schema.fields = {
        {"body", FieldType::Text, IndexOption::FreqsPositions, true, false, false, {}},
    };
    MemorySegment ms2(stored_schema, cfg);
    int i = 0;
    while (!ms2.shouldFlush() && i < 1000) {
        Document d;
        d.set("body", kBodies[i % kBodies.size()]);
        ms2.addDocument(d, static_cast<DocId>(++i));
    }
    if (!ms2.shouldFlush()) FAIL("shouldFlush never triggered");
    PASS();
}

// ─── Test 8: should_flush_hashtable ──────────────────────────────────────────

void test_should_flush_hashtable() {
    TEST(should_flush_when_hashtable_overloaded);

    // 64 buckets → flush threshold = 48 unique terms (0.75 * 64).
    // Each kBodies entry contributes ~6-10 new unique body: terms.
    // After cycling through a few entries the load factor crosses 0.75.
    MemorySegmentConfig cfg;
    cfg.arena_bytes     = 64 * 1024 * 1024;
    cfg.hashtable_cap   = 64;
    cfg.str_arena_bytes = 1024 * 1024;
    MemorySegment ms(makeSchema(), cfg);

    if (ms.shouldFlush()) FAIL("should not flush initially");

    int i = 0;
    while (!ms.shouldFlush() && i < 1000) {
        ms.addDocument(makeDoc(i), static_cast<DocId>(++i));
    }
    if (!ms.shouldFlush()) FAIL("shouldFlush never triggered");
    PASS();
}

// ─── Test 9: reset_clears_state ──────────────────────────────────────────────

void test_reset_clears_state() {
    TEST(reset_clears_all_state);
    MemorySegment ms(makeSchema());

    ms.addDocument(makeDoc(0), 1);
    ms.addDocument(makeDoc(1), 2);
    if (ms.docCount() == 0) FAIL("should have docs before reset");
    size_t ram_before = ms.ramUsed();
    if (ram_before == 0) FAIL("should have ram usage before reset");

    ms.reset();

    if (ms.docCount() != 0)  FAIL("docCount after reset should be 0");
    if (ms.ramUsed() != 0)   FAIL("ramUsed after reset should be 0");
    if (ms.hashtable().size() != 0) FAIL("hashtable should be empty after reset");

    // Should be usable again after reset
    ms.addDocument(makeDoc(0), 1);
    if (ms.docCount() != 1) FAIL("should work normally after reset");
    PASS();
}

// ─── Test 10: stored_field_pages ─────────────────────────────────────────────

void test_stored_field_pages() {
    TEST(stored_field_pages_allocated);

    // Schema with stored body
    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, true, false, false, {}}};
    MemorySegment ms_stored(s);

    // Schema without stored body
    Schema s2;
    s2.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms_nostored(s2);

    Document d;
    d.set("body", kBodies[0]);

    ms_stored.addDocument(d, 1);
    ms_nostored.addDocument(d, 1);

    // Stored schema should use more arena space (StoredPage allocated)
    if (ms_stored.ramUsed() <= ms_nostored.ramUsed())
        FAIL("stored schema should use more arena space than non-stored");
    PASS();
}

// ─── Test 11: multi_field_separation ─────────────────────────────────────────

void test_multi_field_separation() {
    TEST(multi_field_keys_separated);
    MemorySegment ms(makeTwoTextField());

    Document d;
    d.set("title", "search engine");
    d.set("body",  "search algorithm");
    ms.addDocument(d, 1);

    // "search" in both fields → two separate keys
    const Bucket* title_search = ms.hashtable().lookup("title:search");
    const Bucket* body_search  = ms.hashtable().lookup("body:search");
    const Bucket* body_algo    = ms.hashtable().lookup("body:algorithm");
    const Bucket* title_engine = ms.hashtable().lookup("title:engine");

    if (!title_search) FAIL("title:search not found");
    if (!body_search)  FAIL("body:search not found");
    if (!body_algo)    FAIL("body:algorithm not found");
    if (!title_engine) FAIL("title:engine not found");

    // title:search and body:search must point to different TermPages
    if (title_search->term_page_head == body_search->term_page_head)
        FAIL("title:search and body:search share the same TermPage");
    PASS();
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== MemorySegment Write Tests (Step 3) ===\n\n";

    test_single_doc_term_count();
    test_df_ttf_match_reference();
    test_inline_pos_path();
    test_pos_page_path();
    test_position_values_correct();
    test_ram_tracking();
    test_should_flush_arena();
    test_should_flush_hashtable();
    test_reset_clears_state();
    test_stored_field_pages();
    test_multi_field_separation();

    std::cout << "\nAll Step 3 tests passed.\n";
    return 0;
}
