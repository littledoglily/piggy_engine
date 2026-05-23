// tests/memory/test_memory_flush.cpp
//
// Step 5 验证：MemorySegment::flushToDisk —— 内存 segment 落盘后与 SegmentReader 一致性
//
// 测试维度：
//   1. flush_basic           - flush 后 SegmentReader doc_count 与内存一致
//   2. flush_posting         - posting 迭代器 doc_id / tf 顺序与内存完全一致
//   3. flush_term_meta       - getTermMeta df / ttf / upper_bound 与内存一致
//   4. flush_field_len       - fieldDocLen / fieldAvgDocLen 与内存一致
//   5. flush_stored_fields   - 存储字段从磁盘正确还原
//   6. flush_is_alive        - 所有文档在磁盘 segment 中均存活
//   7. flush_then_reset      - reset() 后 doc_count 归零，arena 可复用

#include "memory/memory_segment.h"
#include "memory/memory_segment_reader.h"
#include "index/segment_reader.h"
#include "field/schema.h"
#include "core/types.h"

#include <cassert>
#include <cmath>
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

// ─── Schema & helpers ────────────────────────────────────────────────────────

static Schema makeSchema() {
    Schema s;
    s.fields = {
        {"title",    FieldType::Text,    IndexOption::FreqsPositions, true,  false, false, {}},
        {"body",     FieldType::Text,    IndexOption::FreqsPositions, true,  false, false, {}},
        {"category", FieldType::Keyword, IndexOption::FreqsOnly,      true,  false, false, {}},
    };
    return s;
}

static const std::vector<std::string> kTitles = {
    "distributed systems",
    "machine learning optimization",
    "database query engine",
    "concurrency control",
    "network protocol design",
};
static const std::vector<std::string> kBodies = {
    "distributed systems consistency replication partition fault tolerance",
    "gradient descent optimizer neural network backpropagation training loss",
    "btree index hash join query plan executor optimizer scan",
    "deadlock locking transaction isolation serializable concurrency",
    "tcp http rest api latency throughput bandwidth network protocol",
};
static const std::vector<std::string> kCats =
    {"systems", "ml", "database", "concurrency", "network"};

static Document makeDoc(int i) {
    Document d;
    d.ext_id = static_cast<uint64_t>(1000 + i);
    d.set("title",    kTitles[i % kTitles.size()]);
    d.set("body",     kBodies[i % kBodies.size()]);
    d.set("category", kCats[i % kCats.size()]);
    return d;
}

// ─── TempDir RAII ─────────────────────────────────────────────────────────────

struct TempDir {
    std::string path;
    explicit TempDir(const std::string& p) : path(p) {
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

// ─── Helpers ─────────────────────────────────────────────────────────────────

static std::vector<DocId> drainIter(IPostingIterator& it) {
    std::vector<DocId> v;
    while (!it.isEnd()) { v.push_back(it.docId()); it.next(); }
    return v;
}

static std::vector<uint32_t> drainTf(IPostingIterator& it) {
    std::vector<uint32_t> v;
    while (!it.isEnd()) { v.push_back(it.tf()); it.next(); }
    return v;
}

// ─── Test 1: flush_basic ─────────────────────────────────────────────────────

void test_flush_basic() {
    TEST(flush_basic);

    TempDir td("/tmp/piggy_flush_test_basic");
    Schema s = makeSchema();
    MemorySegment ms(s);

    const int N = 5;
    for (int i = 0; i < N; ++i)
        ms.addDocument(makeDoc(i), static_cast<DocId>(i + 1));

    ms.flushToDisk(td.path, 0);

    SegmentReader sr(td.path, 0);

    if (sr.docCount() != static_cast<uint32_t>(N))
        FAIL("docCount mismatch: expected " + std::to_string(N) +
             ", got " + std::to_string(sr.docCount()));

    PASS();
}

// ─── Test 2: flush_posting ───────────────────────────────────────────────────

void test_flush_posting() {
    TEST(flush_posting);

    TempDir td("/tmp/piggy_flush_test_posting");
    Schema s = makeSchema();
    MemorySegment ms(s);

    const int N = 5;
    for (int i = 0; i < N; ++i)
        ms.addDocument(makeDoc(i), static_cast<DocId>(i + 1));

    MemorySegmentReader mem_reader(ms);
    ms.flushToDisk(td.path, 0);
    SegmentReader disk_reader(td.path, 0);

    const std::string field = "body";
    // Terms that appear across multiple docs
    for (const std::string& term : {"distributed", "optimization", "network", "index"}) {
        auto mem_it  = mem_reader.postingIterator(field, term);
        auto disk_it = disk_reader.postingIterator(field, term);

        std::vector<DocId> mem_docs, disk_docs;
        std::vector<uint32_t> mem_tfs, disk_tfs;

        while (!mem_it->isEnd()) {
            mem_docs.push_back(mem_it->docId());
            mem_tfs.push_back(mem_it->tf());
            mem_it->next();
        }
        while (!disk_it->isEnd()) {
            disk_docs.push_back(disk_it->docId());
            disk_tfs.push_back(disk_it->tf());
            disk_it->next();
        }

        if (mem_docs != disk_docs)
            FAIL("doc_id list mismatch for term '" + term + "'");
        if (mem_tfs != disk_tfs)
            FAIL("tf list mismatch for term '" + term + "'");
    }

    PASS();
}

// ─── Test 3: flush_term_meta ─────────────────────────────────────────────────

void test_flush_term_meta() {
    TEST(flush_term_meta);

    TempDir td("/tmp/piggy_flush_test_meta");
    Schema s = makeSchema();
    MemorySegment ms(s);

    const int N = 5;
    for (int i = 0; i < N; ++i)
        ms.addDocument(makeDoc(i), static_cast<DocId>(i + 1));

    MemorySegmentReader mem_reader(ms);
    ms.flushToDisk(td.path, 0);
    SegmentReader disk_reader(td.path, 0);

    for (const std::string& field : {"body", "title", "category"}) {
        for (const std::string& term : {"systems", "network", "optimization", "deadlock"}) {
            const TermMeta* m_meta  = mem_reader.getTermMeta(field, term);
            const TermMeta* d_meta  = disk_reader.getTermMeta(field, term);

            if (m_meta == nullptr && d_meta == nullptr) continue;
            if (m_meta == nullptr || d_meta == nullptr)
                FAIL("term meta presence mismatch for " + field + ":" + term);

            if (m_meta->doc_freq != d_meta->doc_freq)
                FAIL("df mismatch for " + field + ":" + term +
                     " mem=" + std::to_string(m_meta->doc_freq) +
                     " disk=" + std::to_string(d_meta->doc_freq));

            if (m_meta->total_term_freq != d_meta->total_term_freq)
                FAIL("ttf mismatch for " + field + ":" + term +
                     " mem=" + std::to_string(m_meta->total_term_freq) +
                     " disk=" + std::to_string(d_meta->total_term_freq));

            // Memory upper_bound is conservative (dl=1); disk is exact (actual dl).
            // We only verify disk upper_bound is positive and >= disk max tf_norm floor.
            if (d_meta->upper_bound <= 0.0f)
                FAIL("disk upper_bound should be > 0 for " + field + ":" + term);
            // Memory upper_bound should be >= disk (conservative >= exact).
            if (m_meta->upper_bound < d_meta->upper_bound - 1e-4f)
                FAIL("mem upper_bound should be >= disk upper_bound for " + field + ":" + term +
                     " mem=" + std::to_string(m_meta->upper_bound) +
                     " disk=" + std::to_string(d_meta->upper_bound));
        }
    }

    PASS();
}

// ─── Test 4: flush_field_len ─────────────────────────────────────────────────

void test_flush_field_len() {
    TEST(flush_field_len);

    TempDir td("/tmp/piggy_flush_test_len");
    Schema s = makeSchema();
    MemorySegment ms(s);

    const int N = 5;
    for (int i = 0; i < N; ++i)
        ms.addDocument(makeDoc(i), static_cast<DocId>(i + 1));

    MemorySegmentReader mem_reader(ms);
    ms.flushToDisk(td.path, 0);
    SegmentReader disk_reader(td.path, 0);

    for (const std::string& field : {"body", "title"}) {
        // fieldAvgDocLen
        float m_avg = mem_reader.fieldAvgDocLen(field);
        float d_avg = disk_reader.fieldAvgDocLen(field);
        if (std::fabs(m_avg - d_avg) > 0.1f)
            FAIL("fieldAvgDocLen mismatch for " + field +
                 " mem=" + std::to_string(m_avg) +
                 " disk=" + std::to_string(d_avg));

        // per-doc fieldDocLen
        for (int i = 1; i <= N; ++i) {
            uint32_t m_len = mem_reader.fieldDocLen(field, static_cast<DocId>(i));
            uint32_t d_len = disk_reader.fieldDocLen(field, static_cast<DocId>(i));
            if (m_len != d_len)
                FAIL("fieldDocLen mismatch for " + field + " doc=" + std::to_string(i) +
                     " mem=" + std::to_string(m_len) +
                     " disk=" + std::to_string(d_len));
        }
    }

    PASS();
}

// ─── Test 5: flush_stored_fields ─────────────────────────────────────────────

void test_flush_stored_fields() {
    TEST(flush_stored_fields);

    TempDir td("/tmp/piggy_flush_test_stored");
    Schema s = makeSchema();
    MemorySegment ms(s);

    const int N = 3;
    for (int i = 0; i < N; ++i)
        ms.addDocument(makeDoc(i), static_cast<DocId>(i + 1));

    ms.flushToDisk(td.path, 0);
    SegmentReader disk_reader(td.path, 0);

    for (int i = 0; i < N; ++i) {
        auto stored = disk_reader.readStoredDoc(static_cast<DocId>(i + 1));
        std::string expected_title = kTitles[i % kTitles.size()];
        std::string expected_cat   = kCats[i % kCats.size()];

        auto it = stored.str_fields.find("title");
        if (it == stored.str_fields.end() || it->second != expected_title)
            FAIL("stored title mismatch for doc " + std::to_string(i + 1) +
                 " expected='" + expected_title + "' got='" +
                 (it == stored.str_fields.end() ? "<missing>" : it->second) + "'");

        it = stored.str_fields.find("category");
        if (it == stored.str_fields.end() || it->second != expected_cat)
            FAIL("stored category mismatch for doc " + std::to_string(i + 1));
    }

    PASS();
}

// ─── Test 6: flush_is_alive ───────────────────────────────────────────────────

void test_flush_is_alive() {
    TEST(flush_is_alive);

    TempDir td("/tmp/piggy_flush_test_alive");
    Schema s = makeSchema();
    MemorySegment ms(s);

    const int N = 5;
    for (int i = 0; i < N; ++i)
        ms.addDocument(makeDoc(i), static_cast<DocId>(i + 1));

    ms.flushToDisk(td.path, 0);
    SegmentReader disk_reader(td.path, 0);

    for (int i = 1; i <= N; ++i) {
        if (!disk_reader.isAlive(static_cast<DocId>(i)))
            FAIL("doc " + std::to_string(i) + " should be alive after flush");
    }

    PASS();
}

// ─── Test 7: flush_then_reset ────────────────────────────────────────────────

void test_flush_then_reset() {
    TEST(flush_then_reset);

    TempDir td("/tmp/piggy_flush_test_reset");
    Schema s = makeSchema();
    MemorySegment ms(s);

    const int N = 5;
    for (int i = 0; i < N; ++i)
        ms.addDocument(makeDoc(i), static_cast<DocId>(i + 1));

    if (ms.docCount() != static_cast<uint32_t>(N))
        FAIL("docCount before flush/reset");

    ms.flushToDisk(td.path, 0);
    ms.reset();

    if (ms.docCount() != 0)
        FAIL("docCount should be 0 after reset, got " + std::to_string(ms.docCount()));
    if (ms.ramUsed() != 0)
        FAIL("arena should be empty after reset, ramUsed=" + std::to_string(ms.ramUsed()));

    // Verify flushed data still readable after reset
    SegmentReader disk_reader(td.path, 0);
    if (disk_reader.docCount() != static_cast<uint32_t>(N))
        FAIL("disk docCount should still be " + std::to_string(N) + " after reset");

    PASS();
}

// ─── Test 8: flush_multi_doc_term ─────────────────────────────────────────────

void test_flush_multi_doc_term() {
    TEST(flush_multi_doc_term);

    TempDir td("/tmp/piggy_flush_test_multidoc");
    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    MemorySegment ms(s);

    // "alpha" appears in docs 1,3,5 with tf 1,2,3
    Document d1; d1.set("body", "alpha");            ms.addDocument(d1, 1);
    Document d2; d2.set("body", "beta gamma");       ms.addDocument(d2, 2);
    Document d3; d3.set("body", "alpha alpha");      ms.addDocument(d3, 3);
    Document d4; d4.set("body", "delta");            ms.addDocument(d4, 4);
    Document d5; d5.set("body", "alpha alpha alpha"); ms.addDocument(d5, 5);

    MemorySegmentReader mem_reader(ms);
    ms.flushToDisk(td.path, 0);
    SegmentReader disk_reader(td.path, 0);

    auto mem_it  = mem_reader.postingIterator("body", "alpha");
    auto disk_it = disk_reader.postingIterator("body", "alpha");

    struct Entry { DocId doc; uint32_t tf; };
    auto collect = [](IPostingIterator& it) {
        std::vector<Entry> v;
        while (!it.isEnd()) { v.push_back({it.docId(), it.tf()}); it.next(); }
        return v;
    };

    auto mem_entries  = collect(*mem_it);
    auto disk_entries = collect(*disk_it);

    if (mem_entries.size() != 3 || disk_entries.size() != 3)
        FAIL("expected 3 postings, mem=" + std::to_string(mem_entries.size()) +
             " disk=" + std::to_string(disk_entries.size()));

    for (size_t i = 0; i < mem_entries.size(); ++i) {
        if (mem_entries[i].doc != disk_entries[i].doc)
            FAIL("doc_id mismatch at pos " + std::to_string(i));
        if (mem_entries[i].tf != disk_entries[i].tf)
            FAIL("tf mismatch at pos " + std::to_string(i) +
                 " mem=" + std::to_string(mem_entries[i].tf) +
                 " disk=" + std::to_string(disk_entries[i].tf));
    }

    if (disk_entries[0].tf != 1 || disk_entries[1].tf != 2 || disk_entries[2].tf != 3)
        FAIL("tf values wrong: " + std::to_string(disk_entries[0].tf) + " " +
             std::to_string(disk_entries[1].tf) + " " + std::to_string(disk_entries[2].tf));

    PASS();
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== test_memory_flush ===\n";

    test_flush_basic();
    test_flush_posting();
    test_flush_term_meta();
    test_flush_field_len();
    test_flush_stored_fields();
    test_flush_is_alive();
    test_flush_then_reset();
    test_flush_multi_doc_term();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
