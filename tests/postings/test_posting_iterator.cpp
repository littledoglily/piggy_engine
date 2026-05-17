// tests/postings/test_posting_iterator.cpp
// PostingIterator 功能测试：next / advance / block 跳跃 / blockMaxDocId
//
// 每个测试在 /tmp/test_pi_<suffix> 下独立建段，测试结束清理。
// 正确性基准：readPostingList() 的结果（全量解压后的升序 doc_id 列表）。

#include "postings/posting_iterator.h"
#include "segment/segment_reader.h"
#include "core/index_writer.h"
#include "tokenizer/analyzer.h"
#include "../test_utils.h"

#include <filesystem>
#include <vector>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>

using namespace ii;
namespace fs = std::filesystem;

// ── 辅助 ─────────────────────────────────────────────────────────────────────

static std::string tmpDir(const std::string& suffix) {
    std::string dir = "/tmp/test_pi_" + suffix;
    fs::remove_all(dir);
    return dir;
}

// 返回单词经过 Analyzer 后的形式（用于在 termDict 中准确查找）
static std::string stem(const std::string& word) {
    Analyzer a;
    auto v = a.analyzeQuery(word);
    return v.empty() ? word : v[0];
}

// 在 termDict 中找到 doc_freq == df 的第一个 term
static std::string findTermWithDf(const SegmentReader& seg, uint32_t df) {
    for (const auto& [term, meta] : seg.termDict())
        if (meta.doc_freq == df) return term;
    return "";
}

// 遍历 PostingIterator，收集全部 doc_id
static std::vector<DocId> drainIterator(PostingIterator& iter) {
    std::vector<DocId> result;
    while (!iter.isEnd()) {
        result.push_back(iter.docId());
        iter.next();
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1：next() 顺序遍历与 readPostingList() 完全一致
// ─────────────────────────────────────────────────────────────────────────────
void test_pi_sequential_next() {
    TEST(pi_sequential_next);
    const std::string dir = tmpDir("next");

    {
        IndexWriter writer(dir, 100.0f);
        for (int i = 1; i <= 10; ++i) {
            Document doc;
            doc.doc_id = i;
            doc.title  = "t" + std::to_string(i);
            doc.body   = "mango fruit tropical";
            writer.addDocument(doc);
        }
        writer.commit();
    }

    SegmentReader seg(dir, 0);
    std::string term = stem("mango");
    if (!seg.getTermMeta(term)) term = findTermWithDf(seg, 10);
    if (term.empty()) FAIL("cannot find a term with df=10");

    auto expected = seg.readPostingList(term);
    if (expected.size() != 10) FAIL("unexpected df");

    auto iter = seg.postingIterator(term);
    auto got  = drainIterator(iter);

    if (got != expected) FAIL("next() sequence differs from readPostingList()");
    if (!iter.isEnd()) FAIL("iterator should be exhausted");

    fs::remove_all(dir);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2：advance() 精确命中（target 在 posting list 中）
// ─────────────────────────────────────────────────────────────────────────────
void test_pi_advance_exact() {
    TEST(pi_advance_exact);
    const std::string dir = tmpDir("adv_exact");

    {
        IndexWriter writer(dir, 100.0f);
        for (int i = 1; i <= 10; ++i) {
            Document doc;
            doc.doc_id = i;
            doc.title  = "t" + std::to_string(i);
            doc.body   = "mango fruit tropical";
            writer.addDocument(doc);
        }
        writer.commit();
    }

    SegmentReader seg(dir, 0);
    std::string term = stem("mango");
    if (!seg.getTermMeta(term)) term = findTermWithDf(seg, 10);
    if (term.empty()) FAIL("term not found");

    // advance() 到第一个 doc
    {
        auto iter = seg.postingIterator(term);
        if (!iter.advance(1) || iter.docId() != 1) FAIL("advance(1) failed");
    }
    // advance() 到中间 doc
    {
        auto iter = seg.postingIterator(term);
        if (!iter.advance(5) || iter.docId() != 5) FAIL("advance(5) failed");
        if (!iter.advance(8) || iter.docId() != 8) FAIL("advance(8) from 5 failed");
    }
    // advance() 到最后一个 doc
    {
        auto iter = seg.postingIterator(term);
        if (!iter.advance(10) || iter.docId() != 10) FAIL("advance(10) failed");
    }
    // advance() 已在目标位置（curDoc >= target 直接返回 true，不移动）
    {
        auto iter = seg.postingIterator(term);
        iter.advance(5);
        if (!iter.advance(3) || iter.docId() != 5) FAIL("advance backward should not move");
    }

    fs::remove_all(dir);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3：advance() 到不存在的 doc_id（返回第一个 >= target 的 doc）
// ─────────────────────────────────────────────────────────────────────────────
void test_pi_advance_skip() {
    TEST(pi_advance_skip);
    const std::string dir = tmpDir("adv_skip");

    // 只有奇数 doc_id 的文档包含 "lemon"
    {
        IndexWriter writer(dir, 100.0f);
        for (int i = 1; i <= 20; ++i) {
            Document doc;
            doc.doc_id = i;
            doc.title  = "t" + std::to_string(i);
            doc.body   = (i % 2 == 1) ? "lemon citrus" : "citrus";
            writer.addDocument(doc);
        }
        writer.commit();
    }

    SegmentReader seg(dir, 0);
    std::string term = stem("lemon");
    if (!seg.getTermMeta(term)) FAIL("lemon not found in index");

    auto pl = seg.readPostingList(term);  // {1,3,5,...,19}
    if (pl.size() != 10) FAIL("expected 10 odd-id docs");

    // advance(2) → 跳到 doc 3（第一个 >= 2）
    {
        auto iter = seg.postingIterator(term);
        if (!iter.advance(2) || iter.docId() != 3) FAIL("advance(2) should return doc 3");
    }
    // advance(6) → doc 7
    {
        auto iter = seg.postingIterator(term);
        if (!iter.advance(6) || iter.docId() != 7) FAIL("advance(6) should return doc 7");
    }
    // advance(18) → doc 19
    {
        auto iter = seg.postingIterator(term);
        if (!iter.advance(18) || iter.docId() != 19) FAIL("advance(18) should return doc 19");
    }
    // advance 结果与 lower_bound 完全一致（系统性验证）
    {
        for (DocId target = 1; target <= 21; ++target) {
            auto iter = seg.postingIterator(term);
            bool ok   = iter.advance(target);
            auto lb   = std::lower_bound(pl.begin(), pl.end(), target);
            bool lb_ok = (lb != pl.end());
            if (ok != lb_ok) FAIL("advance/lb mismatch at target=" + std::to_string(target));
            if (ok && iter.docId() != *lb)
                FAIL("advance result wrong at target=" + std::to_string(target));
        }
    }

    fs::remove_all(dir);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4：advance() 超过最后一个 doc → isEnd()
// ─────────────────────────────────────────────────────────────────────────────
void test_pi_advance_past_end() {
    TEST(pi_advance_past_end);
    const std::string dir = tmpDir("past_end");

    {
        IndexWriter writer(dir, 100.0f);
        for (int i = 1; i <= 5; ++i) {
            Document doc;
            doc.doc_id = i;
            doc.title  = "t" + std::to_string(i);
            doc.body   = "mango fruit";
            writer.addDocument(doc);
        }
        writer.commit();
    }

    SegmentReader seg(dir, 0);
    std::string term = stem("mango");
    if (!seg.getTermMeta(term)) term = findTermWithDf(seg, 5);
    if (term.empty()) FAIL("term not found");

    auto iter = seg.postingIterator(term);
    bool ret = iter.advance(100);  // 超出范围
    if (ret) FAIL("advance past end should return false");
    if (!iter.isEnd()) FAIL("iterator should be at end");

    fs::remove_all(dir);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5：多 Block advance()（300 docs → 3 个 Block：128+128+44）
// 验证 SkipList 跨块跳跃的正确性
// ─────────────────────────────────────────────────────────────────────────────
void test_pi_multi_block_advance() {
    TEST(pi_multi_block_advance);
    const std::string dir = tmpDir("multi_blk");

    {
        IndexWriter writer(dir, 100.0f);
        for (int i = 1; i <= 300; ++i) {
            Document doc;
            doc.doc_id = i;
            doc.title  = "t" + std::to_string(i);
            doc.body   = "mango fruit";
            writer.addDocument(doc);
        }
        writer.commit();
    }

    SegmentReader seg(dir, 0);
    std::string term = stem("mango");
    if (!seg.getTermMeta(term)) term = findTermWithDf(seg, 300);
    if (term.empty()) FAIL("term not found");

    auto pl = seg.readPostingList(term);
    if (pl.size() != 300) FAIL("expected 300 docs");

    // Block 边界点验证
    constexpr DocId targets[] = {1, 64, 128, 129, 200, 256, 257, 300};
    for (DocId t : targets) {
        auto iter = seg.postingIterator(term);
        bool ok   = iter.advance(t);
        auto lb   = std::lower_bound(pl.begin(), pl.end(), t);
        if (!ok || iter.docId() != *lb)
            FAIL("multi-block advance failed at target=" + std::to_string(t));
    }

    // 跨块连续 advance（从 block0 → block1 → block2）
    {
        auto iter = seg.postingIterator(term);
        iter.advance(128);
        if (iter.docId() != 128) FAIL("advance(128) should be in block0");
        iter.advance(129);
        if (iter.docId() != 129) FAIL("advance(129) should cross to block1");
        iter.advance(256);
        if (iter.docId() != 256) FAIL("advance(256) last of block1");
        iter.advance(257);
        if (iter.docId() != 257) FAIL("advance(257) should cross to block2");
        iter.advance(300);
        if (iter.docId() != 300) FAIL("advance(300) last doc");
        if (iter.advance(301)) FAIL("advance(301) should be past end");
    }

    fs::remove_all(dir);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6：blockMaxDocId() 随 Block 推进而更新
// ─────────────────────────────────────────────────────────────────────────────
void test_pi_block_max_docid() {
    TEST(pi_block_max_docid);
    const std::string dir = tmpDir("blk_maxdoc");

    {
        IndexWriter writer(dir, 100.0f);
        for (int i = 1; i <= 300; ++i) {
            Document doc;
            doc.doc_id = i;
            doc.title  = "t" + std::to_string(i);
            doc.body   = "mango fruit";
            writer.addDocument(doc);
        }
        writer.commit();
    }

    SegmentReader seg(dir, 0);
    std::string term = stem("mango");
    if (!seg.getTermMeta(term)) term = findTermWithDf(seg, 300);
    if (term.empty()) FAIL("term not found");

    {
        auto iter = seg.postingIterator(term);
        // 初始在 block0（docs 1..128）
        if (iter.blockMaxDocId() != 128)
            FAIL("block0 maxDocId should be 128, got " + std::to_string(iter.blockMaxDocId()));

        // 推进到 block1
        iter.advance(129);
        if (iter.blockMaxDocId() != 256)
            FAIL("block1 maxDocId should be 256, got " + std::to_string(iter.blockMaxDocId()));

        // 推进到 block2（最后一块，44 docs：257..300）
        iter.advance(257);
        if (iter.blockMaxDocId() != 300)
            FAIL("block2 maxDocId should be 300, got " + std::to_string(iter.blockMaxDocId()));
    }

    fs::remove_all(dir);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 7：next() 与 advance() 交替调用的正确性
// ─────────────────────────────────────────────────────────────────────────────
void test_pi_next_and_advance_interleave() {
    TEST(pi_next_advance_interleave);
    const std::string dir = tmpDir("interleave");

    {
        IndexWriter writer(dir, 100.0f);
        for (int i = 1; i <= 300; ++i) {
            Document doc;
            doc.doc_id = i;
            doc.title  = "t" + std::to_string(i);
            doc.body   = "mango fruit";
            writer.addDocument(doc);
        }
        writer.commit();
    }

    SegmentReader seg(dir, 0);
    std::string term = stem("mango");
    if (!seg.getTermMeta(term)) term = findTermWithDf(seg, 300);
    if (term.empty()) FAIL("term not found");

    auto iter = seg.postingIterator(term);

    // next() 推进 5 步 → doc 5
    for (int i = 0; i < 4; ++i) iter.next();
    if (iter.docId() != 5) FAIL("after 4 next() from doc1, expected doc5");

    // advance() 跨块跳跃 → doc 200
    if (!iter.advance(200) || iter.docId() != 200) FAIL("advance(200) failed");

    // next() 继续 → doc 201
    if (!iter.next() || iter.docId() != 201) FAIL("next() after advance(200) should be 201");

    // 再次 advance() 跨块 → doc 257
    if (!iter.advance(257) || iter.docId() != 257) FAIL("advance(257) failed");

    // next() 到 block2 内 → doc 258
    if (!iter.next() || iter.docId() != 258) FAIL("next() after advance(257) should be 258");

    // advance 到末尾
    if (!iter.advance(300) || iter.docId() != 300) FAIL("advance(300) failed");
    if (iter.advance(301)) FAIL("advance past end should fail");

    fs::remove_all(dir);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 8：系统性验证 advance() 与 lower_bound(readPostingList()) 一致（多块）
// ─────────────────────────────────────────────────────────────────────────────
void test_pi_advance_consistency() {
    TEST(pi_advance_consistency);
    const std::string dir = tmpDir("consistency");

    // 稀疏 posting list：每 3 个 doc 中只有第 1 个含 "mango"
    // doc_ids: 1, 4, 7, 10, ... → 共 34 个（300 / 3 向上取整到 300 docs，只取 34 个含 mango）
    // 简化：doc_id % 3 == 1 才加 mango
    {
        IndexWriter writer(dir, 100.0f);
        for (int i = 1; i <= 300; ++i) {
            Document doc;
            doc.doc_id = i;
            doc.title  = "t" + std::to_string(i);
            doc.body   = (i % 3 == 1) ? "mango fruit" : "fruit";
            writer.addDocument(doc);
        }
        writer.commit();
    }

    SegmentReader seg(dir, 0);
    std::string term = stem("mango");
    if (!seg.getTermMeta(term)) FAIL("term not found");

    auto pl = seg.readPostingList(term);  // {1, 4, 7, ..., 298} → 100 docs
    if (pl.size() != 100) FAIL("expected 100 docs (every 3rd), got " + std::to_string(pl.size()));

    // 对每个 target（1..302）验证 advance() == lower_bound(pl)
    for (DocId target = 1; target <= 302; ++target) {
        auto iter = seg.postingIterator(term);
        bool got_ok  = iter.advance(target);
        auto lb      = std::lower_bound(pl.begin(), pl.end(), target);
        bool want_ok = (lb != pl.end());

        if (got_ok != want_ok)
            FAIL("advance/lb ok-flag mismatch at target=" + std::to_string(target));
        if (got_ok && iter.docId() != *lb)
            FAIL("advance result wrong at target=" + std::to_string(target)
                 + " got=" + std::to_string(iter.docId())
                 + " want=" + std::to_string(*lb));
    }

    fs::remove_all(dir);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "═══════════════════════════════════\n";
    std::cout << "  Postings / PostingIterator Tests\n";
    std::cout << "═══════════════════════════════════\n\n";

    test_pi_sequential_next();
    test_pi_advance_exact();
    test_pi_advance_skip();
    test_pi_advance_past_end();
    test_pi_multi_block_advance();
    test_pi_block_max_docid();
    test_pi_next_and_advance_interleave();
    test_pi_advance_consistency();

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
