// tests/memory/test_memory_segment_reader.cpp
//
// Step 4 验证：MemorySegmentReader —— ISegmentReader 内存实现
//
// 测试维度：
//   1. posting_iterator_basic     - 写入多篇文档后 postingIterator next/isEnd 正确
//   2. posting_iterator_advance   - advance(target) 正确跳跃
//   3. posting_iterator_tf        - iterator.tf() 与写入 tf 一致
//   4. getTermMeta_df_ttf         - df / total_term_freq 与写入一致
//   5. getTermMeta_missing        - 未知 term 返回 nullptr
//   6. fieldDocLen                - per-doc per-field token 数正确
//   7. fieldAvgDocLen             - 字段平均 token 数正确
//   8. isAlive_always_true        - 所有 doc_id 均存活
//   9. indexedFieldNames          - 返回被索引字段名，不含 None 字段
//   10. docCount                  - docCount 与写入数量一致
//   11. compare_with_disk_segment - 对比磁盘 SegmentReader：
//         df / ttf / docCount / fieldAvgDocLen 完全一致

#include "memory/memory_segment.h"
#include "memory/memory_segment_reader.h"
#include "field/schema.h"
#include "index/index_writer.h"
#include "index/segment_reader.h"
#include "core/types.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <cmath>

using namespace ii;
using namespace ii::memory;
namespace fs = std::filesystem;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << (msg) << "\n"; assert(false); } while(0)

// ─── Schema & doc helpers ─────────────────────────────────────────────────────

static Schema makeSchema() {
    Schema s;
    s.fields = {
        {"body",     FieldType::Text,    IndexOption::FreqsPositions, true,  false, false, {}},
        {"category", FieldType::Keyword, IndexOption::FreqsOnly,      false, false, false, {}},
    };
    return s;
}

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
    d.set("category", kCats[i % kCats.size()]);
    return d;
}

// ─── Test 1: posting_iterator_basic ──────────────────────────────────────────

void test_posting_iterator_basic() {
    TEST(posting_iterator_basic);

    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    // "alpha" appears in doc 1, 2, 3
    for (int i = 1; i <= 3; ++i) {
        Document d;
        d.set("body", "alpha other");
        ms.addDocument(d, static_cast<DocId>(i));
    }

    MemorySegmentReader reader(ms);
    auto it = reader.postingIterator("body", "alpha");

    if (it->isEnd()) FAIL("iterator should not be at end after 3 docs");

    std::vector<DocId> seen;
    while (!it->isEnd()) {
        seen.push_back(it->docId());
        it->next();
    }

    if (seen.size() != 3) FAIL("expected 3 postings, got " + std::to_string(seen.size()));
    // Must be in ascending order
    for (size_t i = 1; i < seen.size(); ++i)
        if (seen[i] <= seen[i-1]) FAIL("postings not in ascending doc_id order");

    PASS();
}

// ─── Test 2: posting_iterator_advance ────────────────────────────────────────

void test_posting_iterator_advance() {
    TEST(posting_iterator_advance);

    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    for (DocId id : {1u, 3u, 5u, 7u, 9u}) {
        Document d;
        d.set("body", "zephyr");
        ms.addDocument(d, id);
    }

    MemorySegmentReader reader(ms);
    auto it = reader.postingIterator("body", "zephyr");

    // advance to 5 directly
    bool ok = it->advance(5);
    if (!ok)           FAIL("advance(5) should succeed");
    if (it->docId()!=5) FAIL("after advance(5), docId should be 5");

    // advance beyond last
    ok = it->advance(100);
    if (ok)            FAIL("advance(100) should fail (past end)");
    if (!it->isEnd())   FAIL("should be at end after advance past last doc");

    PASS();
}

// ─── Test 3: posting_iterator_tf ─────────────────────────────────────────────

void test_posting_iterator_tf() {
    TEST(posting_iterator_tf);

    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    // doc 1: "alpha" tf=1, doc 2: "alpha alpha alpha" tf=3
    Document d1; d1.set("body", "alpha");
    Document d2; d2.set("body", "alpha alpha alpha");
    ms.addDocument(d1, 1);
    ms.addDocument(d2, 2);

    MemorySegmentReader reader(ms);
    auto it = reader.postingIterator("body", "alpha");

    // sorted: doc1 tf=1, doc2 tf=3
    if (it->isEnd())   FAIL("iterator should not be empty");
    if (it->docId()!=1) FAIL("first doc should be 1");
    if (it->tf()!=1)    FAIL("doc1 tf should be 1, got " + std::to_string(it->tf()));

    it->next();
    if (it->docId()!=2) FAIL("second doc should be 2");
    if (it->tf()!=3)    FAIL("doc2 tf should be 3, got " + std::to_string(it->tf()));

    PASS();
}

// ─── Test 4: getTermMeta df/ttf ──────────────────────────────────────────────

void test_getTermMeta_df_ttf() {
    TEST(getTermMeta_df_ttf);

    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    // "python" in doc1 (tf=2), doc2 (tf=1), doc3 (tf=3)  → df=3, ttf=6
    Document d1; d1.set("body", "python python other");
    Document d2; d2.set("body", "python java");
    Document d3; d3.set("body", "python python python go");
    ms.addDocument(d1, 1);
    ms.addDocument(d2, 2);
    ms.addDocument(d3, 3);

    MemorySegmentReader reader(ms);
    const TermMeta* m = reader.getTermMeta("body", "python");
    if (!m)               FAIL("getTermMeta returned nullptr");
    if (m->doc_freq != 3) FAIL("df should be 3, got " + std::to_string(m->doc_freq));
    if (m->total_term_freq != 6)
        FAIL("ttf should be 6, got " + std::to_string(m->total_term_freq));
    if (m->upper_bound <= 0.0f) FAIL("upper_bound should be positive");

    PASS();
}

// ─── Test 5: getTermMeta missing ─────────────────────────────────────────────

void test_getTermMeta_missing() {
    TEST(getTermMeta_missing_term);

    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    Document d; d.set("body", "hello world");
    ms.addDocument(d, 1);

    MemorySegmentReader reader(ms);
    const TermMeta* m = reader.getTermMeta("body", "nonexistent");
    if (m != nullptr) FAIL("should return nullptr for unknown term");

    PASS();
}

// ─── Test 6: fieldDocLen ─────────────────────────────────────────────────────

void test_fieldDocLen() {
    TEST(fieldDocLen_per_doc);

    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    // doc1: "alpha beta gamma" → 3 tokens
    // doc2: "alpha beta gamma delta epsilon" → 5 tokens
    Document d1; d1.set("body", "alpha beta gamma");
    Document d2; d2.set("body", "alpha beta gamma delta epsilon");
    ms.addDocument(d1, 1);
    ms.addDocument(d2, 2);

    MemorySegmentReader reader(ms);
    uint32_t len1 = reader.fieldDocLen("body", 1);
    uint32_t len2 = reader.fieldDocLen("body", 2);

    if (len1 != 3) FAIL("doc1 body len should be 3, got " + std::to_string(len1));
    if (len2 != 5) FAIL("doc2 body len should be 5, got " + std::to_string(len2));

    PASS();
}

// ─── Test 7: fieldAvgDocLen ──────────────────────────────────────────────────

void test_fieldAvgDocLen() {
    TEST(fieldAvgDocLen_correct);

    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    // doc1: 3 tokens, doc2: 5 tokens → avg = 4.0
    Document d1; d1.set("body", "alpha beta gamma");
    Document d2; d2.set("body", "alpha beta gamma delta epsilon");
    ms.addDocument(d1, 1);
    ms.addDocument(d2, 2);

    MemorySegmentReader reader(ms);
    float avg = reader.fieldAvgDocLen("body");

    if (std::abs(avg - 4.0f) > 0.01f)
        FAIL("fieldAvgDocLen should be 4.0, got " + std::to_string(avg));

    PASS();
}

// ─── Test 8: isAlive always true ─────────────────────────────────────────────

void test_isAlive() {
    TEST(isAlive_always_true);

    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    Document d; d.set("body", "hello");
    ms.addDocument(d, 1);

    MemorySegmentReader reader(ms);
    if (!reader.isAlive(1))   FAIL("doc 1 should be alive");
    if (!reader.isAlive(999)) FAIL("any doc_id should be alive (no soft-delete in memory)");

    PASS();
}

// ─── Test 9: indexedFieldNames ───────────────────────────────────────────────

void test_indexedFieldNames() {
    TEST(indexedFieldNames_correct);

    // Schema: body=indexed, stored_only=None (no index), category=indexed
    Schema s;
    s.fields = {
        {"body",        FieldType::Text,    IndexOption::FreqsPositions, false, false, false, {}},
        {"stored_only", FieldType::Text,    IndexOption::None,           true,  false, false, {}},
        {"category",    FieldType::Keyword, IndexOption::FreqsOnly,      false, false, false, {}},
    };
    MemorySegment ms(s);
    MemorySegmentReader reader(ms);

    const auto& names = reader.indexedFieldNames();
    std::set<std::string> name_set(names.begin(), names.end());

    if (name_set.count("body") == 0)        FAIL("'body' should be in indexedFieldNames");
    if (name_set.count("category") == 0)    FAIL("'category' should be in indexedFieldNames");
    if (name_set.count("stored_only") != 0) FAIL("'stored_only' (None) should NOT be in indexedFieldNames");

    PASS();
}

// ─── Test 10: docCount ───────────────────────────────────────────────────────

void test_docCount() {
    TEST(docCount_matches_written);

    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    MemorySegmentReader reader(ms);
    if (reader.docCount() != 0) FAIL("initial docCount should be 0");

    for (int i = 1; i <= 5; ++i) {
        Document d; d.set("body", "word");
        ms.addDocument(d, static_cast<DocId>(i));
    }
    if (reader.docCount() != 5) FAIL("docCount should be 5");

    PASS();
}

// ─── Test 11: compare with disk SegmentReader ─────────────────────────────────

void test_compare_with_disk_segment() {
    TEST(compare_with_disk_segment_df_ttf);

    const int N_DOCS = 500;
    auto schema = makeSchema();

    // Build in-memory
    MemorySegment ms(schema);
    for (int i = 0; i < N_DOCS; ++i)
        ms.addDocument(makeDoc(i), static_cast<DocId>(i + 1));
    MemorySegmentReader mem_reader(ms);

    // Build on-disk reference
    fs::path tmpdir = "/tmp/piggy_ms_reader_ref";
    fs::remove_all(tmpdir);
    {
        IndexWriter writer(tmpdir.string(), 512.0f, schema);
        DocId id = 0;
        for (int i = 0; i < N_DOCS; ++i) {
            Document d = makeDoc(i);
            d.doc_id = ++id;
            writer.addDocument(d);
        }
        writer.commit();
    }
    SegmentReader disk_reader(tmpdir.string(), 0);

    // Compare docCount
    if (mem_reader.docCount() != disk_reader.docCount())
        FAIL("docCount mismatch: mem=" + std::to_string(mem_reader.docCount())
             + " disk=" + std::to_string(disk_reader.docCount()));

    // Collect all terms from memory segment hashtable
    int mismatches = 0;
    ms.hashtable().forEach([&](const Bucket& b) {
        std::string key(ms.hashtable().termString(b));
        // key = "field:term"
        size_t colon = key.find(':');
        if (colon == std::string::npos) return;
        std::string field = key.substr(0, colon);
        std::string term  = key.substr(colon + 1);

        const TermMeta* mm = mem_reader.getTermMeta(field, term);
        const TermMeta* dm = disk_reader.getTermMeta(field, term);

        if (!mm || !dm) {
            std::cout << "\n  MISSING meta for " << key;
            ++mismatches; return;
        }
        if (mm->doc_freq != dm->doc_freq) {
            std::cout << "\n  df mismatch for " << key
                      << ": mem=" << mm->doc_freq << " disk=" << dm->doc_freq;
            ++mismatches;
        }
        if (mm->total_term_freq != dm->total_term_freq) {
            std::cout << "\n  ttf mismatch for " << key
                      << ": mem=" << mm->total_term_freq << " disk=" << dm->total_term_freq;
            ++mismatches;
        }
    });

    if (mismatches > 0) FAIL(std::to_string(mismatches) + " mismatch(es)");

    // Compare fieldAvgDocLen
    for (const auto& fname : mem_reader.indexedFieldNames()) {
        float m_avg = mem_reader.fieldAvgDocLen(fname);
        float d_avg = disk_reader.fieldAvgDocLen(fname);
        if (std::abs(m_avg - d_avg) > 0.5f)  // allow minor rounding
            FAIL("fieldAvgDocLen mismatch for field=" + fname
                 + " mem=" + std::to_string(m_avg) + " disk=" + std::to_string(d_avg));
    }

    fs::remove_all(tmpdir);
    PASS();
}

// ─── main ────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== MemorySegmentReader Tests (Step 4) ===\n";
    test_posting_iterator_basic();
    test_posting_iterator_advance();
    test_posting_iterator_tf();
    test_getTermMeta_df_ttf();
    test_getTermMeta_missing();
    test_fieldDocLen();
    test_fieldAvgDocLen();
    test_isAlive();
    test_indexedFieldNames();
    test_docCount();
    test_compare_with_disk_segment();
    std::cout << "\nAll Step 4 tests passed.\n";
}
