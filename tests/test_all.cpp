// ─────────────────────────────────────────────────────────────────────────────
// test_all.cpp  —  单元测试（无第三方框架，仅 assert）
// ─────────────────────────────────────────────────────────────────────────────
#include "tokenizer/analyzer.h"
#include "postings/pfor_delta.h"
#include "postings/skiplist.h"
#include "postings/posting_list.h"
#include <filesystem>
#include "core/index_writer.h"
#include "query/index_searcher.h"
#include "segment/segment_merger.h"
#include <cassert>
#include <iostream>
#include <numeric>

using namespace ii;

#define TEST(name) do { std::cout << "[TEST] " #name "... "; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << msg << "\n"; assert(false); } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Analyzer Tests
// ─────────────────────────────────────────────────────────────────────────────

void test_analyzer_basic() {
    TEST(analyzer_basic);
    Analyzer a;
    auto toks = a.analyze(1, "Python programming for beginners");
    // "for" は停用词，应被过滤
    bool has_for = false;
    for (const auto& t : toks) if (t.term == "for") has_for = true;
    if (has_for) FAIL("stop word 'for' not filtered");
    if (toks.empty()) FAIL("no tokens");
    PASS();
}

void test_analyzer_stemming() {
    TEST(analyzer_stemming);
    Analyzer a;
    // "programming" → stem → "program" (去 "ing")
    auto toks = a.analyze(1, "programming programs");
    std::string first = toks.empty() ? "" : toks[0].term;
    // 词干后应包含 "program"
    bool found = false;
    for (const auto& t : toks) {
        if (t.term.find("program") != std::string::npos) { found = true; break; }
    }
    if (!found) FAIL("stemming failed for 'programming'");
    PASS();
}

void test_analyzer_query() {
    TEST(analyzer_query);
    Analyzer a;
    auto terms = a.analyzeQuery("Python Tutorial AND Java");
    // "and" 是停用词，应被过滤
    for (const auto& t : terms) {
        if (t == "and") FAIL("'and' should be filtered");
    }
    if (terms.size() < 2) FAIL("too few query terms");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// PForDelta Tests
// ─────────────────────────────────────────────────────────────────────────────

void test_pfor_small() {
    TEST(pfor_small_list);
    std::vector<DocId> docs = {1, 3, 5, 10, 20, 50};
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes);
    auto decoded = PForDelta::decompress(
        compressed.data(), compressed.size(), docs.size());
    if (decoded != docs) FAIL("small list round-trip failed");
    PASS();
}

void test_pfor_128() {
    TEST(pfor_exact_128);
    // 生成恰好 128 个升序 doc_id
    std::vector<DocId> docs;
    docs.reserve(128);
    DocId cur = 1;
    for (int i = 0; i < 128; ++i) {
        docs.push_back(cur);
        cur += (i % 5) + 1;
    }
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes);
    assert(snodes.size() == 1);  // 恰好 1 个 Block
    auto decoded = PForDelta::decompress(
        compressed.data(), compressed.size(), docs.size());
    if (decoded != docs) FAIL("128 doc round-trip failed");
    PASS();
}

void test_pfor_multi_block() {
    TEST(pfor_multi_block);
    // 256 个 doc_id → 2 个 Block
    std::vector<DocId> docs;
    docs.reserve(256);
    DocId cur = 1;
    for (int i = 0; i < 256; ++i) { docs.push_back(cur); cur += 2; }
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes);
    assert(snodes.size() == 2);
    auto decoded = PForDelta::decompress(
        compressed.data(), compressed.size(), docs.size());
    if (decoded != docs) FAIL("multi-block round-trip failed");
    PASS();
}

void test_pfor_exceptions() {
    TEST(pfor_exceptions);
    // 制造大跨度 delta（超出 b 位）
    std::vector<DocId> docs = {1, 2, 3, 4, 5, 1000, 1001, 1002, 5000, 5001};
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes);
    auto decoded = PForDelta::decompress(
        compressed.data(), compressed.size(), docs.size());
    if (decoded != docs) FAIL("exception handling failed");
    PASS();
}

void test_pfor_compression_ratio() {
    TEST(pfor_compression_ratio);
    // 生成 128 个 delta=1~3 的 doc_id（高压缩率场景）
    std::vector<DocId> docs;
    DocId cur = 1;
    for (int i = 0; i < 128; ++i) { docs.push_back(cur); cur += (i%3)+1; }
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes);
    size_t original = docs.size() * sizeof(DocId);
    // 压缩后应 < 原始的 50%
    if (compressed.size() >= original / 2) {
        FAIL("poor compression ratio: " + std::to_string(compressed.size())
             + " vs " + std::to_string(original));
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// SkipList Tests
// ─────────────────────────────────────────────────────────────────────────────

void test_skiplist_find() {
    TEST(skiplist_find);
    std::vector<SkipNode> nodes;
    for (int i = 0; i < 5; ++i) {
        SkipNode sn;
        sn.max_doc_id  = (DocId)((i + 1) * 128);
        sn.byte_offset = (uint64_t)(i * 200);
        sn.max_score   = 0.5f;
        sn.doc_count   = 128;
        nodes.push_back(sn);
    }
    SkipList sl(nodes);

    auto r1 = sl.find(1);
    assert(r1.block_index == 0);
    assert(r1.byte_offset == 0);

    auto r2 = sl.find(128);
    assert(r2.block_index == 0);

    auto r3 = sl.find(129);
    assert(r3.block_index == 1);

    auto r4 = sl.find(640);
    assert(r4.block_index == 4);

    auto r5 = sl.find(641);  // 超出范围
    assert(r5.byte_offset == UINT64_MAX);

    PASS();
}

void test_skiplist_serialize() {
    TEST(skiplist_serialize);
    std::vector<SkipNode> nodes;
    for (int i = 0; i < 3; ++i) {
        SkipNode sn;
        sn.max_doc_id  = (i + 1) * 100;
        sn.byte_offset = i * 80;
        sn.max_score   = 0.3f;
        sn.doc_count   = 100;
        nodes.push_back(sn);
    }
    SkipList sl(nodes);
    auto bytes = sl.serialize();
    auto sl2   = SkipList::deserialize(bytes.data(), bytes.size());

    assert(sl2.size() == sl.size());
    for (size_t i = 0; i < sl.size(); ++i) {
        assert(sl.node(i).max_doc_id  == sl2.node(i).max_doc_id);
        assert(sl.node(i).byte_offset == sl2.node(i).byte_offset);
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// PostingList Tests
// ─────────────────────────────────────────────────────────────────────────────

void test_posting_list_append() {
    TEST(posting_list_append);
    PostingList pl;
    pl.append(1, 0);
    pl.append(1, 5);  // 同一 doc 第二次出现
    pl.append(3, 2);
    pl.append(5, 1);

    assert(pl.size() == 3);  // 3 个唯一 doc

    const auto& entries = pl.entries();
    assert(entries[0].doc_id == 1);
    assert(entries[0].tf     == 2);
    assert(entries[0].positions.size() == 2);
    assert(entries[1].doc_id == 3);
    assert(entries[1].tf     == 1);

    auto ids = pl.docIds();
    assert(ids.size() == 3);
    assert(ids[0] == 1 && ids[1] == 3 && ids[2] == 5);
    PASS();
}

void test_in_memory_index() {
    TEST(in_memory_index);
    InMemoryIndex idx;
    Token t1{"python", 1, 0, 0, 6};
    Token t2{"java",   1, 1, 7, 11};
    Token t3{"python", 2, 0, 0, 6};
    Token t4{"python", 2, 5, 30, 36};

    idx.addToken(t1);
    idx.addToken(t2);
    idx.addToken(t3);
    idx.addToken(t4);

    assert(idx.termCount() == 2);

    const PostingList* pl_py = idx.getPostingList("python");
    assert(pl_py != nullptr);
    assert(pl_py->size() == 2);   // doc 1 和 doc 2

    const PostingList* pl_ja = idx.getPostingList("java");
    assert(pl_ja != nullptr);
    assert(pl_ja->size() == 1);

    auto terms = idx.sortedTerms();
    assert(terms[0] == "java");
    assert(terms[1] == "python");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────


void test_soft_delete_and_merge() {
    TEST(soft_delete_and_merge);
    const std::string dir = "/tmp/ii_merge_test";
    std::filesystem::remove_all(dir);

    // 写入 10 篇文档
    {
        ii::IndexWriter writer(dir, 0.01f); // 极小 buffer，必然多次 flush
        for (int i = 1; i <= 10; ++i) {
            ii::Document doc;
            doc.doc_id   = i;
            doc.set("title", "Doc " + std::to_string(i));
            doc.set("body", "python tutorial data science machine learning " + std::to_string(i));
            doc.set("category", "test");
            writer.addDocument(doc);
        }
        writer.commit();
    }

    // 软删除 doc 1, 3, 5
    {
        ii::SegmentMerger merger(dir);
        int deleted = merger.softDeleteBatch({1, 3, 5});
        assert(deleted == 3);
        assert(!merger.isAlive(1));
        assert(!merger.isAlive(3));
        assert( merger.isAlive(2));
        assert( merger.isAlive(4));

        // 合并
        auto stats = merger.mergeAll(50);
        assert(stats.deleted_doc_count == 3);
        assert(stats.output_doc_count  == 7);
    }

    // 合并后验证：用新 Segment 搜索，不应包含已删除文档
    {
        ii::IndexSearcher searcher(dir);
        auto results = searcher.search("python", 10, ii::QueryMode::OR);
        for (const auto& r : results) {
            // 被删除文档的原 title 包含 "Doc 1", "Doc 3", "Doc 5"
            // 合并后 doc_id 重编号，通过 title 验证
            bool is_deleted_title =
                r.title == "Doc 1" || r.title == "Doc 3" || r.title == "Doc 5";
            if (is_deleted_title) FAIL("deleted doc appeared in results");
        }
    }
    std::filesystem::remove_all(dir);
    PASS();
}

void test_merge_tf_and_positions() {
    TEST(merge_tf_and_positions);
    const std::string dir = "/tmp/ii_tf_pos_test";
    std::filesystem::remove_all(dir);

    // Segment 0: doc1 含 "python" 3 次，doc2 含 "python" 1 次
    // Segment 1: doc3 含 "python" 2 次
    // merge 后验证 tf 和 positions 均被正确保留
    {
        ii::IndexWriter writer(dir, 100.0f);

        ii::Document d1;
        d1.doc_id   = 1;
        d1.set("title", "Multi Python");
        d1.set("body", "python python python");
        d1.set("category", "test");
        writer.addDocument(d1);

        ii::Document d2;
        d2.doc_id   = 2;
        d2.set("title", "Single Python");
        d2.set("body", "python tutorial");
        d2.set("category", "test");
        writer.addDocument(d2);

        writer.flush();  // → segment 0

        ii::Document d3;
        d3.doc_id   = 3;
        d3.set("title", "Double Python");
        d3.set("body", "python python guide");
        d3.set("category", "test");
        writer.addDocument(d3);

        writer.commit();  // → segment 1
    }

    // 合并两个 segment
    uint32_t merged_seg_id = 0;
    {
        ii::SegmentMerger merger(dir);
        auto ids = merger.activeSegmentIds();
        if (ids.size() < 2) FAIL("expected 2 segments before merge");
        auto stats = merger.mergeAll(99);
        merged_seg_id = stats.output_segment_id;
        if (stats.output_doc_count != 3) FAIL("expected 3 docs after merge");
    }

    // 用 SegmentReader 直接验证 .pos 文件内容
    {
        ii::SegmentReader reader(dir, merged_seg_id);

        // 找到 "python" 的 pos entries
        // 分词后 "python" 不会被词干化，直接查
        auto entries = reader.readPosEntries("python");
        if (entries.size() != 3) FAIL("expected 3 docs with python, got " + std::to_string(entries.size()));

        // 按 doc_id 排序（merge 后 doc_id 从 1 重新编号）
        std::sort(entries.begin(), entries.end(),
                  [](const ii::PostingEntry& a, const ii::PostingEntry& b) {
                      return a.doc_id < b.doc_id;
                  });

        // analyzer 拼接 title+body 分析：
        //   doc1: "Multi Python python python python" → python 出现 4 次 (positions 1,2,3,4)
        //   doc2: "Single Python python tutorial"    → python 出现 2 次 (positions 1,2)
        //   doc3: "Double Python python python guide"→ python 出现 3 次 (positions 1,2,3)
        uint32_t expected_tf[3] = {4, 2, 3};
        for (int i = 0; i < 3; ++i) {
            if (entries[i].tf != expected_tf[i])
                FAIL("doc" + std::to_string(i+1) + " tf expected "
                     + std::to_string(expected_tf[i]) + ", got "
                     + std::to_string(entries[i].tf));
            if (entries[i].positions.size() != expected_tf[i])
                FAIL("doc" + std::to_string(i+1) + " positions count mismatch");
            // positions 必须严格递增
            for (uint32_t j = 1; j < entries[i].positions.size(); ++j) {
                if (entries[i].positions[j] <= entries[i].positions[j-1])
                    FAIL("doc" + std::to_string(i+1) + " positions not ascending");
            }
        }

        // total_term_freq = 4+2+3 = 9
        const ii::TermMeta* meta = reader.getTermMeta("python");
        if (!meta) FAIL("python not found in term dict");
        if (meta->total_term_freq != 9)
            FAIL("total_term_freq expected 9, got " + std::to_string(meta->total_term_freq));
    }

    std::filesystem::remove_all(dir);
    PASS();
}

int main() {
    std::cout << "═══════════════════════════════════\n";
    std::cout << "  Inverted Index Unit Tests\n";
    std::cout << "═══════════════════════════════════\n\n";

    // Analyzer
    test_analyzer_basic();
    test_analyzer_stemming();
    test_analyzer_query();

    // PForDelta
    test_pfor_small();
    test_pfor_128();
    test_pfor_multi_block();
    test_pfor_exceptions();
    test_pfor_compression_ratio();

    // SkipList
    test_skiplist_find();
    test_skiplist_serialize();

    // PostingList / InMemoryIndex
    test_posting_list_append();
    test_in_memory_index();

    // Merger integration test
    test_soft_delete_and_merge();
    test_merge_tf_and_positions();

    std::cout << "\n═══════════════════════════════════\n";
    std::cout << "  All tests PASSED!\n";
    std::cout << "═══════════════════════════════════\n";
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// SegmentMerger Tests  (需要磁盘，集成测试)
// ─────────────────────────────────────────────────────────────────────────────

