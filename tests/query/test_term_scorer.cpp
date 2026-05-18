// tests/query/test_term_scorer.cpp
// TermScorer 正确性测试：
//   1. docId 遍历序列与 PostingIterator 一致
//   2. maxScore() == TermMeta::upper_bound × IDF
//   3. term 不存在时 isEnd()==true
//   4. advance() 行为与 PostingIterator 一致

#include "query/term_scorer.h"
#include "query/scorer_context.h"
#include "segment/segment_reader.h"
#include "core/index_writer.h"
#include "tokenizer/analyzer.h"
#include "../test_utils.h"

#include <filesystem>
#include <vector>
#include <cmath>
#include <cassert>
#include <iostream>
#include <string>

using namespace ii;
namespace fs = std::filesystem;

static std::string tmpDir(const std::string& s) {
    std::string d = "/tmp/test_ts_" + s;
    fs::remove_all(d);
    return d;
}

// Analyzer stem 辅助
static std::string stem(const std::string& w) {
    Analyzer a;
    auto v = a.analyzeQuery(w);
    return v.empty() ? w : v[0];
}

// 收集 TermScorer 所有 doc_id
static std::vector<DocId> drain(TermScorer& ts) {
    std::vector<DocId> r;
    while (!ts.isEnd()) { r.push_back(ts.docId()); ts.next(); }
    return r;
}

// 简单 IDF：ln(N / df + 1) + 1，与 SegmentReader::bm25Score 一致
static float calcIdf(uint32_t N, uint32_t df) {
    return std::log(static_cast<float>(N) / static_cast<float>(df) + 1.0f) + 1.0f;
}

// ─────────────────────────────────────────────────────────────────────────────

static void test_docid_sequence() {
    TEST("TermScorer docId 序列与 PostingIterator 一致");

    auto dir = tmpDir("seq");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 50; ++i) {
            Document doc;
            doc.doc_id = static_cast<DocId>(i);
            doc.set("body", i % 3 == 0 ? "python machine" : "python");
            doc.set("title", i % 5 == 0 ? "python tutorial" : "tutorial");
            w.addDocument(doc);
        }
        w.flush();
    }

    SegmentReader seg(dir, 0);
    const std::string field = "body";
    const std::string term  = stem("python");

    // 基准：全量解压
    auto baseline = seg.readPostingList(field + "_" + term);
    // 实际用 per-field API
    auto iter = seg.postingIterator(field, term);
    std::vector<DocId> from_iter;
    while (!iter.isEnd()) { from_iter.push_back(iter.docId()); iter.next(); }

    // 构造 ScorerContext（term_idfs 填充 field:term → IDF）
    const TermMeta* meta = seg.getTermMeta(field, term);
    assert(meta && "term 应存在");
    uint32_t total = seg.docCount();
    float idf = calcIdf(total, meta->doc_freq);
    std::unordered_map<std::string, float> term_idfs;
    term_idfs[field + ":" + term] = idf;

    ScorerContext ctx{seg, term_idfs, total, nullptr};
    TermScorer ts(field, term, ctx);

    auto got = drain(ts);
    if (got != from_iter) FAIL("docId 序列与 PostingIterator 不一致");
    PASS();

    fs::remove_all(dir);
}

static void test_max_score() {
    TEST("TermScorer maxScore() == upper_bound × IDF");

    auto dir = tmpDir("ub");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 30; ++i) {
            Document doc;
            doc.doc_id = static_cast<DocId>(i);
            doc.set("body", "learning machine learning");
            w.addDocument(doc);
        }
        w.flush();
    }

    SegmentReader seg(dir, 0);
    const std::string field = "body";
    const std::string term  = stem("learning");

    const TermMeta* meta = seg.getTermMeta(field, term);
    assert(meta && "term 应存在");

    uint32_t total = seg.docCount();
    float idf = calcIdf(total, meta->doc_freq);
    std::unordered_map<std::string, float> term_idfs;
    term_idfs[field + ":" + term] = idf;

    ScorerContext ctx{seg, term_idfs, total, nullptr};
    TermScorer ts(field, term, ctx);

    float expected = meta->upper_bound * idf;
    float got      = ts.maxScore();
    if (std::abs(got - expected) > 1e-5f)
        FAIL("maxScore 与 upper_bound × IDF 不符");
    PASS();

    fs::remove_all(dir);
}

static void test_missing_term() {
    TEST("TermScorer 不存在的 term isEnd()==true");

    auto dir = tmpDir("miss");
    {
        IndexWriter w(dir, 8);
        Document doc; doc.doc_id = 1; doc.set("body", "hello world"); w.addDocument(doc);
        w.flush();
    }

    SegmentReader seg(dir, 0);
    std::unordered_map<std::string, float> term_idfs;
    ScorerContext ctx{seg, term_idfs, seg.docCount(), nullptr};
    TermScorer ts("body", "nonexistent", ctx);

    if (!ts.isEnd()) FAIL("不存在的 term 应 isEnd()==true");
    PASS();

    fs::remove_all(dir);
}

static void test_advance() {
    TEST("TermScorer advance() 行为与 PostingIterator 一致");

    auto dir = tmpDir("adv");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 60; ++i) {
            Document doc;
            doc.doc_id = static_cast<DocId>(i);
            doc.set("body", i % 2 == 0 ? "python java" : "java");
            w.addDocument(doc);
        }
        w.flush();
    }

    SegmentReader seg(dir, 0);
    const std::string field = "body";
    const std::string term  = stem("python");

    const TermMeta* meta = seg.getTermMeta(field, term);
    assert(meta && "term 应存在");
    uint32_t total = seg.docCount();
    float idf = calcIdf(total, meta->doc_freq);
    std::unordered_map<std::string, float> term_idfs;
    term_idfs[field + ":" + term] = idf;

    ScorerContext ctx{seg, term_idfs, total, nullptr};

    // advance 到中间位置，验证结果 >= target
    DocId target = 20;
    TermScorer ts(field, term, ctx);
    bool found = ts.advance(target);

    auto ref_iter = seg.postingIterator(field, term);
    bool ref_found = ref_iter.advance(target);

    if (found != ref_found) FAIL("advance 返回值不一致");
    if (found && ts.docId() != ref_iter.docId()) FAIL("advance 后 docId 不一致");
    PASS();

    fs::remove_all(dir);
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    test_docid_sequence();
    test_max_score();
    test_missing_term();
    test_advance();

    std::cout << "\n═══════════════════════════════════\n";
    std::cout << "  All TermScorer tests PASSED!\n";
    std::cout << "═══════════════════════════════════\n";
    return 0;
}
