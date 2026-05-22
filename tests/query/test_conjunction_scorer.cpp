// tests/query/test_conjunction_scorer.cpp
//
// ConjunctionScorer 单元测试，覆盖以下情形：
//   1. 基础交集正确性（结果与暴力 N-way 基准一致）
//   2. 空交集（某 term 在任何 doc 都不出现）
//   3. 单子节点（退化为 TermScorer 遍历）
//   4. 全量命中（所有 doc 都包含所有 term）
//   5. advance() 跳跃语义
//   6. 跨 Block 边界（>128 doc）
//   7. score() = Σ children scores
//   8. maxScore() = Σ children maxScores
//   9. isEnd() / next() 耗尽后行为稳健
//  10. 子节点输入顺序无关性（内部重排保证正确）

#include "query/conjunction_scorer.h"
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
#include <cassert>
#include <iostream>
#include <algorithm>

using namespace ii;
namespace fs = std::filesystem;

// ── 辅助 ────────────────────────────────────────────────────────────────────

static std::string tmpDir(const std::string& s) {
    std::string d = "/tmp/test_cs_" + s;
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

static std::vector<DocId> drain(ConjunctionScorer& cs) {
    std::vector<DocId> r;
    while (!cs.isEnd()) { r.push_back(cs.docId()); cs.next(); }
    return r;
}

using TermIdfs = std::unordered_map<std::string, float>;

static TermIdfs buildIdfs(const SegmentReader& seg, uint32_t total,
                          const std::vector<std::pair<std::string,std::string>>& fterms) {
    TermIdfs m;
    for (const auto& [f, t] : fterms) {
        const TermMeta* meta = seg.getTermMeta(f, t);
        if (!meta) continue;
        m[f + ":" + t] = calcIdf(total, meta->doc_freq);
    }
    return m;
}

// 通过 PostingIterator 收集 field:term 的全部 doc_id
static std::vector<DocId> readAll(const SegmentReader& seg,
                                  const std::string& field, const std::string& term) {
    auto it = seg.postingIterator(field, term);
    std::vector<DocId> r;
    while (!it.isEnd()) { r.push_back(it.docId()); it.next(); }
    return r;
}

// 暴力求多路交集
static std::vector<DocId> bruteAND(const SegmentReader& seg,
                                   const std::vector<std::pair<std::string,std::string>>& fterms) {
    if (fterms.empty()) return {};
    auto pl0 = readAll(seg, fterms[0].first, fterms[0].second);
    std::set<DocId> result(pl0.begin(), pl0.end());
    for (size_t i = 1; i < fterms.size(); ++i) {
        auto pl = readAll(seg, fterms[i].first, fterms[i].second);
        std::set<DocId> s(pl.begin(), pl.end());
        std::set<DocId> inter;
        for (DocId d : result) if (s.count(d)) inter.insert(d);
        result = inter;
    }
    return {result.begin(), result.end()};
}

static std::vector<std::unique_ptr<Scorer>> makeTermScorers(
    const std::vector<std::pair<std::string,std::string>>& fterms,
    const ScorerContext& ctx)
{
    std::vector<std::unique_ptr<Scorer>> v;
    for (const auto& [f, t] : fterms)
        v.push_back(std::make_unique<TermScorer>(f, t, ctx));
    return v;
}

// ── 测试 1：基础交集正确性 ──────────────────────────────────────────────────

static void test_basic_intersection() {
    TEST("ConjunctionScorer 基础交集正确性");
    auto dir = tmpDir("basic");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 80; ++i) {
            Document doc;
            doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 2 == 0) body += " machine";
            if (i % 3 == 0) body += " deep";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", stem("python")},
        {"body", stem("machine")},
        {"body", stem("deep")},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    ConjunctionScorer cs(makeTermScorers(fterms, ctx));
    auto got      = drain(cs);
    auto expected = bruteAND(seg, fterms);

    if (got != expected) FAIL("交集结果与暴力基准不一致");
    if (got.empty()) FAIL("预期有命中 doc（6 的倍数）");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 2：空交集 ──────────────────────────────────────────────────────────

static void test_empty_intersection() {
    TEST("ConjunctionScorer 空交集（term 不存在）");
    auto dir = tmpDir("empty");
    {
        IndexWriter w(dir, 8);
        Document doc; doc.doc_id = 1; doc.set("body", "python machine");
        w.addDocument(doc);
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", stem("python")},
        {"body", "nonexistent"},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    ConjunctionScorer cs(makeTermScorers(fterms, ctx));
    if (!cs.isEnd()) FAIL("空交集应立即 isEnd()==true");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 3：单子节点退化 ────────────────────────────────────────────────────

static void test_single_child() {
    TEST("ConjunctionScorer 单子节点退化遍历");
    auto dir = tmpDir("single");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 10; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body", "python");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t = stem("python");
    TermIdfs idfs;
    idfs["body:" + t] = calcIdf(total, seg.getTermMeta("body", t)->doc_freq);
    ScorerContext ctx{seg, idfs, total, nullptr};

    std::vector<std::unique_ptr<Scorer>> v;
    v.push_back(std::make_unique<TermScorer>("body", t, ctx));
    ConjunctionScorer cs(std::move(v));

    auto got = drain(cs);
    auto ref = readAll(seg, "body", t);
    if (got != ref) FAIL("单子节点结果应等于 TermScorer 全量遍历");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 4：全量命中 ────────────────────────────────────────────────────────

static void test_all_docs_match() {
    TEST("ConjunctionScorer 全量命中（所有 doc 含所有 term）");
    auto dir = tmpDir("all");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 20; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body", "python machine deep");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", stem("python")},
        {"body", stem("machine")},
        {"body", stem("deep")},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    ConjunctionScorer cs(makeTermScorers(fterms, ctx));
    auto got = drain(cs);

    if (static_cast<int>(got.size()) != 20) FAIL("全量命中应返回 20 个 doc");
    for (int i = 0; i < 20; ++i)
        if (got[i] != static_cast<DocId>(i + 1)) FAIL("全量命中 doc_id 序列错误");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 5：advance() 跳跃语义 ─────────────────────────────────────────────

static void test_advance() {
    TEST("ConjunctionScorer advance() 跳跃语义");
    auto dir = tmpDir("adv");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 60; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 2 == 0) body += " machine";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", stem("python")},
        {"body", stem("machine")},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    auto ref = bruteAND(seg, fterms);  // 全量基准

    DocId target = 30;
    ConjunctionScorer cs(makeTermScorers(fterms, ctx));
    bool found = cs.advance(target);

    auto it = std::lower_bound(ref.begin(), ref.end(), target);
    bool ref_found = (it != ref.end());

    if (found != ref_found) FAIL("advance() 返回值与基准不一致");
    if (found && cs.docId() != *it) FAIL("advance() 后 docId 与基准不一致");

    std::vector<DocId> rest;
    while (!cs.isEnd()) { rest.push_back(cs.docId()); cs.next(); }
    std::vector<DocId> ref_rest(it, ref.end());
    if (rest != ref_rest) FAIL("advance() 后的遍历结果与基准不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 6：跨 Block 边界（>128 doc）──────────────────────────────────────

static void test_cross_block() {
    TEST("ConjunctionScorer 跨 Block 边界（256 doc）");
    auto dir = tmpDir("block");
    {
        IndexWriter w(dir, 32);
        for (int i = 1; i <= 256; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 2 == 1) body += " learn";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", stem("python")},
        {"body", stem("learn")},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    ConjunctionScorer cs(makeTermScorers(fterms, ctx));
    auto got      = drain(cs);
    auto expected = bruteAND(seg, fterms);

    if (got != expected) FAIL("跨 Block 边界时交集结果错误");
    if (got.size() != 128) FAIL("预期 128 个命中（奇数 doc）");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 7：score() = Σ children scores ───────────────────────────────────

static void test_score_sum() {
    TEST("ConjunctionScorer score() = Σ children scores");
    auto dir = tmpDir("score");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 5; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body", "python machine");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t1 = stem("python"), t2 = stem("machine");
    TermIdfs idfs;
    idfs["body:" + t1] = calcIdf(total, seg.getTermMeta("body", t1)->doc_freq);
    idfs["body:" + t2] = calcIdf(total, seg.getTermMeta("body", t2)->doc_freq);
    ScorerContext ctx{seg, idfs, total, nullptr};

    // 单独计算 doc_id=1 时各子节点得分
    TermScorer ts1("body", t1, ctx), ts2("body", t2, ctx);
    float expected = ts1.score() + ts2.score();

    std::vector<std::unique_ptr<Scorer>> v;
    v.push_back(std::make_unique<TermScorer>("body", t1, ctx));
    v.push_back(std::make_unique<TermScorer>("body", t2, ctx));
    ConjunctionScorer cs(std::move(v));

    if (cs.isEnd()) FAIL("预期有命中 doc");
    float got = cs.score();
    if (std::abs(got - expected) > 1e-4f) FAIL("score() 不等于 Σ children scores");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 8：maxScore() = Σ children maxScores ─────────────────────────────

static void test_max_score() {
    TEST("ConjunctionScorer maxScore() = Σ children maxScores");
    auto dir = tmpDir("maxs");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 10; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body", "python deep learning");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t1 = stem("python"), t2 = stem("deep"), t3 = stem("learning");
    TermIdfs idfs;
    for (const auto& t : {t1, t2, t3}) {
        const TermMeta* m = seg.getTermMeta("body", t);
        if (m) idfs["body:" + t] = calcIdf(total, m->doc_freq);
    }
    ScorerContext ctx{seg, idfs, total, nullptr};

    TermScorer ts1("body", t1, ctx), ts2("body", t2, ctx), ts3("body", t3, ctx);
    float expected = ts1.maxScore() + ts2.maxScore() + ts3.maxScore();

    std::vector<std::unique_ptr<Scorer>> v;
    v.push_back(std::make_unique<TermScorer>("body", t1, ctx));
    v.push_back(std::make_unique<TermScorer>("body", t2, ctx));
    v.push_back(std::make_unique<TermScorer>("body", t3, ctx));
    ConjunctionScorer cs(std::move(v));

    if (std::abs(cs.maxScore() - expected) > 1e-5f)
        FAIL("maxScore() 不等于 Σ children maxScores");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 9：耗尽后行为稳健 ─────────────────────────────────────────────────

static void test_exhaustion() {
    TEST("ConjunctionScorer next() 耗尽后 isEnd() 保持 true");
    auto dir = tmpDir("exhaust");
    {
        IndexWriter w(dir, 8);
        Document doc; doc.doc_id = 1; doc.set("body", "python machine");
        w.addDocument(doc);
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t1 = stem("python"), t2 = stem("machine");
    TermIdfs idfs;
    idfs["body:" + t1] = calcIdf(total, seg.getTermMeta("body", t1)->doc_freq);
    idfs["body:" + t2] = calcIdf(total, seg.getTermMeta("body", t2)->doc_freq);
    ScorerContext ctx{seg, idfs, total, nullptr};

    std::vector<std::unique_ptr<Scorer>> v;
    v.push_back(std::make_unique<TermScorer>("body", t1, ctx));
    v.push_back(std::make_unique<TermScorer>("body", t2, ctx));
    ConjunctionScorer cs(std::move(v));

    if (cs.isEnd()) FAIL("应有 1 个命中 doc");
    cs.next();
    if (!cs.isEnd()) FAIL("耗尽后应 isEnd()==true");
    // 继续调用不应崩溃，且保持 isEnd
    cs.next();
    cs.advance(999);
    if (!cs.isEnd()) FAIL("耗尽后 isEnd() 应保持 true");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 10：子节点输入顺序无关性 ─────────────────────────────────────────

static void test_order_independence() {
    TEST("ConjunctionScorer 子节点输入顺序无关（内部重排）");
    auto dir = tmpDir("order");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 40; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 2 == 0) body += " machine";
            if (i % 3 == 0) body += " learn";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();

    // python df 最大，learn df 最小；两种输入顺序应得到相同结果
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", stem("python")},
        {"body", stem("machine")},
        {"body", stem("learn")},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    // 顺序 A：python, machine, learn（df 降序，最差顺序）
    ConjunctionScorer cs1(makeTermScorers(fterms, ctx));
    auto got1 = drain(cs1);

    // 顺序 B：learn, machine, python（df 升序）
    std::vector<std::pair<std::string,std::string>> fterms_rev = {
        {"body", stem("learn")},
        {"body", stem("machine")},
        {"body", stem("python")},
    };
    ConjunctionScorer cs2(makeTermScorers(fterms_rev, ctx));
    auto got2 = drain(cs2);

    if (got1 != got2) FAIL("不同输入顺序应得到相同交集结果");
    auto expected = bruteAND(seg, fterms);
    if (got1 != expected) FAIL("交集结果与暴力基准不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 入口 ────────────────────────────────────────────────────────────────────

int main() {
    test_basic_intersection();
    test_empty_intersection();
    test_single_child();
    test_all_docs_match();
    test_advance();
    test_cross_block();
    test_score_sum();
    test_max_score();
    test_exhaustion();
    test_order_independence();

    std::cout << "\n═══════════════════════════════════\n";
    std::cout << "  All ConjunctionScorer tests PASSED!\n";
    std::cout << "═══════════════════════════════════\n";
    return 0;
}
