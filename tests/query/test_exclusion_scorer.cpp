// tests/query/test_exclusion_scorer.cpp
//
// ExclusionScorer 单元测试，覆盖以下情形：
//   1. 基础过滤正确性（结果 = main 集合 − excluded 集合）
//   2. 全部过滤（main 与 excluded 完全重叠 → 空结果）
//   3. 无过滤（excluded term 不存在 → 结果等于 main 全量）
//   4. advance() 跳跃语义
//   5. score() 等于 main 的得分（excluded 不贡献）
//   6. maxScore() 等于 main 的 maxScore
//   7. 耗尽后 isEnd() 保持 true，再调 next()/advance() 不崩溃
//   8. 跨 Block 边界（>128 doc）

#include "query/exclusion_scorer.h"
#include "query/term_scorer.h"
#include "query/scorer_context.h"
#include "index/segment_reader.h"
#include "index/index_writer.h"
#include "analysis/analyzer.h"
#include "../test_utils.h"

#include <filesystem>
#include <vector>
#include <set>
#include <cmath>
#include <iostream>
#include <algorithm>

using namespace ii;
namespace fs = std::filesystem;

// ── 辅助 ─────────────────────────────────────────────────────────────────────

static std::string tmpDir(const std::string& s) {
    std::string d = "/tmp/test_es_" + s;
    fs::remove_all(d);
    return d;
}

static std::string stem(const std::string& w) {
    Analyzer a;
    auto v = a.analyzeQuery(w);
    return v.empty() ? w : v[0];
}

static float calcIdf(uint32_t N, uint32_t df) {
    return std::log(static_cast<float>(N) / static_cast<float>(df) + 1.f) + 1.f;
}

using TermIdfs = std::unordered_map<std::string, float>;

static TermIdfs buildIdf1(const SegmentReader& seg, uint32_t total,
                          const std::string& field, const std::string& term) {
    TermIdfs m;
    const TermMeta* meta = seg.getTermMeta(field, term);
    if (meta) m[field + ":" + term] = calcIdf(total, meta->doc_freq);
    return m;
}

static TermIdfs mergeIdfs(const TermIdfs& a, const TermIdfs& b) {
    TermIdfs m = a;
    m.insert(b.begin(), b.end());
    return m;
}

// 通过 PostingIterator 收集所有 doc_id
static std::vector<DocId> readAll(const SegmentReader& seg,
                                  const std::string& field, const std::string& term) {
    auto it = seg.postingIterator(field, term);
    std::vector<DocId> r;
    while (!it->isEnd()) { r.push_back(it->docId()); it->next(); }
    return r;
}

// 暴力差集：main_docs − excluded_docs
static std::vector<DocId> bruteExclude(const std::vector<DocId>& main_docs,
                                        const std::vector<DocId>& excl_docs) {
    std::set<DocId> excl(excl_docs.begin(), excl_docs.end());
    std::vector<DocId> r;
    for (DocId d : main_docs)
        if (!excl.count(d)) r.push_back(d);
    return r;
}

static std::vector<DocId> drain(ExclusionScorer& es) {
    std::vector<DocId> r;
    while (!es.isEnd()) { r.push_back(es.docId()); es.next(); }
    return r;
}

// ── 测试 1：基础过滤正确性 ───────────────────────────────────────────────────

static void test_basic_exclusion() {
    TEST("ExclusionScorer 基础过滤正确性");
    auto dir = tmpDir("basic");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 60; ++i) {
            Document doc;
            doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 3 == 0) body += " spam";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t_main = stem("python");
    const std::string t_excl = stem("spam");

    auto idfs_m = buildIdf1(seg, total, "body", t_main);
    auto idfs_e = buildIdf1(seg, total, "body", t_excl);
    auto idfs   = mergeIdfs(idfs_m, idfs_e);
    ScorerContext ctx{seg, idfs, total, nullptr};

    auto main_scorer = std::make_unique<TermScorer>("body", t_main, ctx);
    auto excl_scorer = std::make_unique<TermScorer>("body", t_excl, ctx);
    ExclusionScorer es(std::move(main_scorer), std::move(excl_scorer));

    auto got = drain(es);
    auto main_docs = readAll(seg, "body", t_main);
    auto excl_docs = readAll(seg, "body", t_excl);
    auto expected  = bruteExclude(main_docs, excl_docs);

    if (got != expected) FAIL("过滤结果与暴力差集基准不一致");
    if (got.empty()) FAIL("预期有非 spam doc（1, 2, 4, 5, …）");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 2：全部过滤（main ⊆ excluded）──────────────────────────────────────

static void test_all_excluded() {
    TEST("ExclusionScorer 全部过滤（main 完全被 excluded 覆盖）");
    auto dir = tmpDir("allexcl");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 10; ++i) {
            Document doc;
            doc.doc_id = static_cast<DocId>(i);
            doc.set("body", "python spam");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t_main = stem("python");
    const std::string t_excl = stem("spam");

    auto idfs_m = buildIdf1(seg, total, "body", t_main);
    auto idfs_e = buildIdf1(seg, total, "body", t_excl);
    auto idfs   = mergeIdfs(idfs_m, idfs_e);
    ScorerContext ctx{seg, idfs, total, nullptr};

    ExclusionScorer es(
        std::make_unique<TermScorer>("body", t_main, ctx),
        std::make_unique<TermScorer>("body", t_excl, ctx));

    if (!es.isEnd()) FAIL("全部被过滤时应立即 isEnd()==true");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 3：无过滤（excluded term 不存在）────────────────────────────────────

static void test_no_exclusion() {
    TEST("ExclusionScorer 无过滤（excluded term 不在索引中）");
    auto dir = tmpDir("noexcl");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 20; ++i) {
            Document doc;
            doc.doc_id = static_cast<DocId>(i);
            doc.set("body", "python");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t_main = stem("python");

    TermIdfs idfs;
    idfs["body:" + t_main] = calcIdf(total, seg.getTermMeta("body", t_main)->doc_freq);
    ScorerContext ctx{seg, idfs, total, nullptr};

    ExclusionScorer es(
        std::make_unique<TermScorer>("body", t_main, ctx),
        std::make_unique<TermScorer>("body", "nonexistent", ctx));

    auto got = drain(es);
    auto expected = readAll(seg, "body", t_main);
    if (got != expected) FAIL("无 excluded term 时结果应等于 main 全量");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 4：advance() 跳跃语义 ───────────────────────────────────────────────

static void test_advance() {
    TEST("ExclusionScorer advance() 跳跃语义");
    auto dir = tmpDir("adv");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 60; ++i) {
            Document doc;
            doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 5 == 0) body += " spam";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t_main = stem("python");
    const std::string t_excl = stem("spam");

    auto idfs_m = buildIdf1(seg, total, "body", t_main);
    auto idfs_e = buildIdf1(seg, total, "body", t_excl);
    auto idfs   = mergeIdfs(idfs_m, idfs_e);
    ScorerContext ctx{seg, idfs, total, nullptr};

    // 计算全量基准
    auto main_docs = readAll(seg, "body", t_main);
    auto excl_docs = readAll(seg, "body", t_excl);
    auto full_ref  = bruteExclude(main_docs, excl_docs);

    DocId target = 30;
    ExclusionScorer es(
        std::make_unique<TermScorer>("body", t_main, ctx),
        std::make_unique<TermScorer>("body", t_excl, ctx));

    bool found = es.advance(target);

    auto it = std::lower_bound(full_ref.begin(), full_ref.end(), target);
    bool ref_found = (it != full_ref.end());

    if (found != ref_found) FAIL("advance() 返回值与基准不一致");
    if (found && es.docId() != *it) FAIL("advance() 后 docId 与基准不一致");

    std::vector<DocId> rest;
    while (!es.isEnd()) { rest.push_back(es.docId()); es.next(); }
    std::vector<DocId> ref_rest(it, full_ref.end());
    if (rest != ref_rest) FAIL("advance() 后的遍历结果与基准不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 5：score() 等于 main 得分 ───────────────────────────────────────────

static void test_score_equals_main() {
    TEST("ExclusionScorer score() 等于 main 得分（excluded 不贡献）");
    auto dir = tmpDir("score");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 5; ++i) {
            Document doc;
            doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 2 == 0) body += " spam";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t_main = stem("python");
    const std::string t_excl = stem("spam");

    auto idfs_m = buildIdf1(seg, total, "body", t_main);
    auto idfs_e = buildIdf1(seg, total, "body", t_excl);
    auto idfs   = mergeIdfs(idfs_m, idfs_e);
    ScorerContext ctx{seg, idfs, total, nullptr};

    // 参考：doc_id=1 时 main 的得分
    TermScorer ref_ts("body", t_main, ctx);
    float expected_score = ref_ts.score();

    ExclusionScorer es(
        std::make_unique<TermScorer>("body", t_main, ctx),
        std::make_unique<TermScorer>("body", t_excl, ctx));

    if (es.isEnd()) FAIL("doc_id=1 应通过过滤");
    if (es.docId() != 1) FAIL("第一个非 spam doc 应为 doc_id=1");
    if (std::abs(es.score() - expected_score) > 1e-4f)
        FAIL("score() 应等于 main 的 BM25 得分");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 6：maxScore() 等于 main 的 maxScore ─────────────────────────────────

static void test_max_score_equals_main() {
    TEST("ExclusionScorer maxScore() 等于 main 的 maxScore");
    auto dir = tmpDir("maxs");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 10; ++i) {
            Document doc;
            doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 3 == 0) body += " spam";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t_main = stem("python");
    const std::string t_excl = stem("spam");

    auto idfs_m = buildIdf1(seg, total, "body", t_main);
    auto idfs_e = buildIdf1(seg, total, "body", t_excl);
    auto idfs   = mergeIdfs(idfs_m, idfs_e);
    ScorerContext ctx{seg, idfs, total, nullptr};

    TermScorer ref_main("body", t_main, ctx);
    float expected_max = ref_main.maxScore();

    ExclusionScorer es(
        std::make_unique<TermScorer>("body", t_main, ctx),
        std::make_unique<TermScorer>("body", t_excl, ctx));

    if (std::abs(es.maxScore() - expected_max) > 1e-5f)
        FAIL("maxScore() 应等于 main 的 maxScore");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 7：耗尽后行为稳健 ───────────────────────────────────────────────────

static void test_exhaustion() {
    TEST("ExclusionScorer 耗尽后 isEnd() 保持 true");
    auto dir = tmpDir("exhaust");
    {
        IndexWriter w(dir, 8);
        Document doc;
        doc.doc_id = 1;
        doc.set("body", "python");
        w.addDocument(doc);
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t_main = stem("python");

    TermIdfs idfs;
    idfs["body:" + t_main] = calcIdf(total, seg.getTermMeta("body", t_main)->doc_freq);
    ScorerContext ctx{seg, idfs, total, nullptr};

    ExclusionScorer es(
        std::make_unique<TermScorer>("body", t_main, ctx),
        std::make_unique<TermScorer>("body", "nonexistent", ctx));

    if (es.isEnd()) FAIL("应有 1 个命中 doc");
    es.next();
    if (!es.isEnd()) FAIL("耗尽后 isEnd() 应为 true");
    // 继续调用不应崩溃
    es.next();
    es.advance(999);
    if (!es.isEnd()) FAIL("耗尽后 isEnd() 应保持 true");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 8：跨 Block 边界 ────────────────────────────────────────────────────

static void test_cross_block() {
    TEST("ExclusionScorer 跨 Block 边界（256 doc）");
    auto dir = tmpDir("block");
    {
        IndexWriter w(dir, 32);
        for (int i = 1; i <= 256; ++i) {
            Document doc;
            doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 7 == 0) body += " spam";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t_main = stem("python");
    const std::string t_excl = stem("spam");

    auto idfs_m = buildIdf1(seg, total, "body", t_main);
    auto idfs_e = buildIdf1(seg, total, "body", t_excl);
    auto idfs   = mergeIdfs(idfs_m, idfs_e);
    ScorerContext ctx{seg, idfs, total, nullptr};

    ExclusionScorer es(
        std::make_unique<TermScorer>("body", t_main, ctx),
        std::make_unique<TermScorer>("body", t_excl, ctx));

    auto got = drain(es);
    auto main_docs = readAll(seg, "body", t_main);
    auto excl_docs = readAll(seg, "body", t_excl);
    auto expected  = bruteExclude(main_docs, excl_docs);

    if (got != expected) FAIL("跨 Block 边界时过滤结果错误");
    if (got.empty()) FAIL("预期有非 spam doc");
    PASS();
    fs::remove_all(dir);
}

// ── 入口 ─────────────────────────────────────────────────────────────────────

int main() {
    test_basic_exclusion();
    test_all_excluded();
    test_no_exclusion();
    test_advance();
    test_score_equals_main();
    test_max_score_equals_main();
    test_exhaustion();
    test_cross_block();

    std::cout << "\n═══════════════════════════════════\n";
    std::cout << "  All ExclusionScorer tests PASSED!\n";
    std::cout << "═══════════════════════════════════\n";
    return 0;
}
