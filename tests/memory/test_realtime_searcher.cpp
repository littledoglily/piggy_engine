// tests/memory/test_realtime_searcher.cpp
//
// Step 6 验证：IndexSearcher 集成实时 Segment
//
// 测试维度：
//   1. rt_search_hits         - 写入文档未 flush，attachRealtime 后直接搜索命中
//   2. rt_search_multi_term   - AND 查询：仅命中同时包含两个词的文档
//   3. rt_search_stored       - 搜索结果 stored 字段（title/body）正确还原
//   4. rt_flush_reload        - flush 后 reload 磁盘 segment，搜索结果与 rt 期间一致
//   5. rt_mixed               - 内存 segment + 磁盘 segment 混合查询，结果为并集
//   6. rt_detach              - detachRealtime() 后不再命中内存文档
//   7. rt_attach_new          - attachRealtime 替换新 segment，旧结果不再可见

#include "memory/memory_segment.h"
#include "memory/memory_segment_reader.h"
#include "query/index_searcher.h"
#include "index/segment_reader.h"
#include "field/schema.h"
#include "core/types.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace ii;
using namespace ii::memory;
namespace fs = std::filesystem;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << (msg) << "\n"; assert(false); } while(0)

// ─── helpers ─────────────────────────────────────────────────────────────────

struct TempDir {
    std::string path;
    explicit TempDir(const std::string& p) : path(p) { fs::create_directories(path); }
    ~TempDir() { fs::remove_all(path); }
};

static Schema makeSchema() {
    Schema s;
    s.fields = {
        {"title",    FieldType::Text,    IndexOption::FreqsPositions, true,  false, false, {}},
        {"body",     FieldType::Text,    IndexOption::FreqsPositions, true,  false, false, {}},
        {"category", FieldType::Keyword, IndexOption::FreqsOnly,      true,  false, false, {}},
    };
    return s;
}

static Document makeDoc(const std::string& title, const std::string& body,
                         const std::string& cat = "tech", uint64_t ext_id = 0)
{
    Document d;
    d.ext_id = ext_id;
    d.set("title", title);
    d.set("body",  body);
    d.set("category", cat);
    return d;
}

// 创建一个空磁盘索引目录（IndexSearcher 构造需要合法目录）
static void makeEmptyIndex(const std::string& dir) {
    fs::create_directories(dir);
}

// ─── Test 1: rt_search_hits ───────────────────────────────────────────────────

void test_rt_search_hits() {
    TEST(rt_search_hits);

    TempDir td("/tmp/piggy_rt_test_hits");
    makeEmptyIndex(td.path);

    Schema s = makeSchema();
    MemorySegment ms(s);

    ms.addDocument(makeDoc("distributed systems", "replication consistency partition", "systems", 1), 1);
    ms.addDocument(makeDoc("machine learning", "gradient descent neural network", "ml", 2), 2);
    ms.addDocument(makeDoc("database engine", "btree index hash join query", "db", 3), 3);

    auto rt = std::make_shared<MemorySegmentReader>(ms);
    IndexSearcher searcher(td.path);
    searcher.attachRealtime(rt);

    auto results = searcher.search("body:replication", 10, QueryMode::OR);
    if (results.empty())
        FAIL("expected hits for 'replication', got none");
    bool found = false;
    for (const auto& r : results) if (r.doc_id == 1) { found = true; break; }
    if (!found) FAIL("doc 1 (replication) not in results");

    PASS();
}

// ─── Test 2: rt_search_multi_term ─────────────────────────────────────────────

void test_rt_search_multi_term() {
    TEST(rt_search_multi_term);

    TempDir td("/tmp/piggy_rt_test_multi");
    makeEmptyIndex(td.path);

    Schema s = makeSchema();
    MemorySegment ms(s);

    // doc 1: both alpha and beta  |  doc 2: only alpha  |  doc 3: only beta
    ms.addDocument(makeDoc("alpha beta", "alpha beta gamma"), 1);
    ms.addDocument(makeDoc("alpha only", "alpha gamma delta"), 2);
    ms.addDocument(makeDoc("beta only",  "beta gamma epsilon"), 3);

    auto rt = std::make_shared<MemorySegmentReader>(ms);
    IndexSearcher searcher(td.path);
    searcher.attachRealtime(rt);

    // AND query: only doc 1 has both alpha AND beta in body
    auto results = searcher.search("body:alpha body:beta", 10, QueryMode::AND);
    if (results.size() != 1)
        FAIL("AND query should return 1 result, got " + std::to_string(results.size()));
    if (results[0].doc_id != 1)
        FAIL("AND query should return doc 1, got " + std::to_string(results[0].doc_id));

    PASS();
}

// ─── Test 3: rt_search_stored ─────────────────────────────────────────────────

void test_rt_search_stored() {
    TEST(rt_search_stored);

    TempDir td("/tmp/piggy_rt_test_stored");
    makeEmptyIndex(td.path);

    Schema s = makeSchema();
    MemorySegment ms(s);

    ms.addDocument(makeDoc("search engine", "inverted index ranking bm25 scoring", "ir", 42), 1);

    auto rt = std::make_shared<MemorySegmentReader>(ms);
    IndexSearcher searcher(td.path);
    searcher.attachRealtime(rt);

    auto results = searcher.search("body:inverted", 10, QueryMode::OR);
    if (results.empty()) FAIL("expected hit for 'inverted'");

    const auto& r = results[0];
    if (r.title != "search engine")
        FAIL("stored title mismatch: got '" + r.title + "'");
    if (r.ext_id != 42)
        FAIL("ext_id mismatch: got " + std::to_string(r.ext_id));

    PASS();
}

// ─── Test 4: rt_flush_reload ──────────────────────────────────────────────────

void test_rt_flush_reload() {
    TEST(rt_flush_reload);

    TempDir td("/tmp/piggy_rt_test_flush_reload");
    makeEmptyIndex(td.path);

    Schema s = makeSchema();
    MemorySegment ms(s);

    ms.addDocument(makeDoc("neural network", "backpropagation gradient descent optimizer"), 1);
    ms.addDocument(makeDoc("deep learning", "convolutional recurrent transformer attention"), 2);

    // Search before flush (realtime path)
    auto rt = std::make_shared<MemorySegmentReader>(ms);
    IndexSearcher searcher1(td.path);
    searcher1.attachRealtime(rt);

    auto results_rt = searcher1.search("body:gradient", 10, QueryMode::OR);
    if (results_rt.empty()) FAIL("rt: expected hit for 'gradient'");
    std::vector<DocId> rt_docs;
    for (const auto& r : results_rt) rt_docs.push_back(r.doc_id);

    // Flush to disk
    ms.flushToDisk(td.path, 0);

    // Reload from disk (no rt segment)
    IndexSearcher searcher2(td.path);
    auto results_disk = searcher2.search("body:gradient", 10, QueryMode::OR);
    if (results_disk.empty()) FAIL("disk: expected hit for 'gradient'");

    // Results should be the same doc_ids
    std::vector<DocId> disk_docs;
    for (const auto& r : results_disk) disk_docs.push_back(r.doc_id);

    std::sort(rt_docs.begin(), rt_docs.end());
    std::sort(disk_docs.begin(), disk_docs.end());
    if (rt_docs != disk_docs)
        FAIL("rt and disk results differ");

    PASS();
}

// ─── Test 5: rt_mixed ──────────────────────────────────────────────────────────

void test_rt_mixed() {
    TEST(rt_mixed);

    TempDir td("/tmp/piggy_rt_test_mixed");

    Schema s = makeSchema();
    MemorySegment ms_flush(s);
    MemorySegment ms_rt(s);

    // Flush doc 1 to disk
    ms_flush.addDocument(makeDoc("disk document", "disk storage persistence durability"), 1);
    ms_flush.flushToDisk(td.path, 0);

    // Keep doc 2 in memory
    ms_rt.addDocument(makeDoc("memory document", "memory cache volatile fast"), 1);

    auto rt = std::make_shared<MemorySegmentReader>(ms_rt);
    IndexSearcher searcher(td.path);
    searcher.attachRealtime(rt);

    // "disk" should come from disk segment
    auto disk_hits = searcher.search("body:disk", 10, QueryMode::OR);
    if (disk_hits.empty()) FAIL("expected hit for 'disk' from disk segment");

    // "volatile" should come from memory segment
    auto mem_hits = searcher.search("body:volatile", 10, QueryMode::OR);
    if (mem_hits.empty()) FAIL("expected hit for 'volatile' from memory segment");

    // OR query for both should return 2 results (one from each segment)
    auto both_hits = searcher.search("body:disk body:volatile", 10, QueryMode::OR);
    if (both_hits.size() < 2)
        FAIL("expected >=2 results for mixed query, got " + std::to_string(both_hits.size()));

    PASS();
}

// ─── Test 6: rt_detach ────────────────────────────────────────────────────────

void test_rt_detach() {
    TEST(rt_detach);

    TempDir td("/tmp/piggy_rt_test_detach");
    makeEmptyIndex(td.path);

    Schema s = makeSchema();
    MemorySegment ms(s);

    ms.addDocument(makeDoc("ephemeral doc", "ephemeral transient temporary"), 1);

    auto rt = std::make_shared<MemorySegmentReader>(ms);
    IndexSearcher searcher(td.path);
    searcher.attachRealtime(rt);

    auto before = searcher.search("body:ephemeral", 10, QueryMode::OR);
    if (before.empty()) FAIL("should find doc before detach");

    searcher.detachRealtime();
    auto after = searcher.search("body:ephemeral", 10, QueryMode::OR);
    if (!after.empty()) FAIL("should not find doc after detach");

    PASS();
}

// ─── Test 7: rt_attach_new ────────────────────────────────────────────────────

void test_rt_attach_new() {
    TEST(rt_attach_new);

    TempDir td("/tmp/piggy_rt_test_attach_new");
    makeEmptyIndex(td.path);

    Schema s = makeSchema();
    MemorySegment ms1(s), ms2(s);

    // Use unique words that the analyzer won't split or conflate
    ms1.addDocument(makeDoc("first segment",  "zorblaxtoken"), 1);
    ms2.addDocument(makeDoc("second segment", "blorbfuztoken"), 1);

    auto rt1 = std::make_shared<MemorySegmentReader>(ms1);
    auto rt2 = std::make_shared<MemorySegmentReader>(ms2);

    IndexSearcher searcher(td.path);
    searcher.attachRealtime(rt1);

    auto r1 = searcher.search("body:zorblaxtoken", 10, QueryMode::OR);
    if (r1.empty()) FAIL("rt1: should find zorblaxtoken");

    // Switch to rt2
    searcher.attachRealtime(rt2);

    auto r2_zor = searcher.search("body:zorblaxtoken", 10, QueryMode::OR);
    if (!r2_zor.empty()) FAIL("after switch, zorblaxtoken should not be found");

    auto r2_blorb = searcher.search("body:blorbfuztoken", 10, QueryMode::OR);
    if (r2_blorb.empty()) FAIL("rt2: should find blorbfuztoken");

    PASS();
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== test_realtime_searcher ===\n";

    test_rt_search_hits();
    test_rt_search_multi_term();
    test_rt_search_stored();
    test_rt_flush_reload();
    test_rt_mixed();
    test_rt_detach();
    test_rt_attach_new();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
