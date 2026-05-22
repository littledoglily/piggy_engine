// tests/query/test_wand_scorer.cpp
//
// WANDScorer 单元测试，覆盖以下情形：
//   1.  Top-K 召回正确性（结果集与暴力枚举完全一致）
//   2.  空游标（term 不存在）→ 空结果
//   3.  top_k >= 总命中数 → 返回所有命中
//   4.  单子节点退化（一个 TermScorer）
//   5.  跨 Block 边界（> 128 doc），BlockMax 跳跃后结果仍正确
//   6.  maxScore() = Σ children maxScores
//   7.  Scorer 接口 next() / docId() / isEnd() 顺序迭代
//   8.  Scorer 接口 advance() 跳跃语义
//   9.  top_k = 0 → 空结果（边界异常）
//  10.  collectTopK() 返回结果按得分降序排列

#include "query/wand_scorer.h"
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
#include <algorithm>
#include <iostream>

using namespace ii;
namespace fs = std::filesystem;

// ── 辅助函数 ─────────────────────────────────────────────────────────────────

static std::string tmpDir(const std::string& s) {
    std::string d = "/tmp/test_wand_" + s;
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

static TermIdfs buildIdfs(const SegmentReader& seg, uint32_t total,
                          const std::vector<std::pair<std::string, std::string>>& fterms) {
    TermIdfs m;
    for (const auto& [f, t] : fterms) {
        const TermMeta* meta = seg.getTermMeta(f, t);
        if (!meta) continue;
        m[f + ":" + t] = calcIdf(total, meta->doc_freq);
    }
    return m;
}

// 收集 field:term 的全部 doc_id
static std::vector<DocId> readAll(const SegmentReader& seg,
                                   const std::string& field,
                                   const std::string& term) {
    auto it = seg.postingIterator(field, term);
    std::vector<DocId> r;
    while (!it.isEnd()) { r.push_back(it.docId()); it.next(); }
    return r;
}

// 暴力 OR Top-K：
//   1. 收集所有命中 doc_id 的并集
//   2. 对每个 doc 计算得分 = Σ TermScorer.score()（只对含该 term 的 doc 累加）
//   3. 取得分最高的 K 个 doc_id（集合，不要求顺序）
static std::set<DocId> bruteForceTopKIds(
    const SegmentReader& seg,
    const ScorerContext& ctx,
    const std::vector<std::pair<std::string, std::string>>& fterms,
    int top_k)
{
    // 并集
    std::set<DocId> all_docs;
    for (const auto& [f, t] : fterms) {
        for (DocId d : readAll(seg, f, t)) all_docs.insert(d);
    }

    // 打分
    std::vector<std::pair<float, DocId>> scored;
    for (DocId d : all_docs) {
        float s = 0.f;
        for (const auto& [f, t] : fterms) {
            TermScorer ts(f, t, ctx);
            if (!ts.isEnd() && ts.advance(d) && ts.docId() == d)
                s += ts.score();
        }
        scored.push_back({s, d});
    }

    // Sort by score desc, then docId asc as tiebreaker.
    // WAND processes cursors in docId ascending order, so for equal scores the lower
    // docId is encountered first and preferred. Brute force must match that ordering.
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) {
                  if (std::abs(a.first - b.first) > 1e-6f) return a.first > b.first;
                  return a.second < b.second;  // docId asc as tiebreaker
              });

    std::set<DocId> result;
    int cnt = 0;
    for (const auto& [s, d] : scored) {
        if (cnt++ >= top_k) break;
        result.insert(d);
    }
    return result;
}

static std::vector<std::unique_ptr<Scorer>> makeTermScorers(
    const std::vector<std::pair<std::string, std::string>>& fterms,
    const ScorerContext& ctx)
{
    std::vector<std::unique_ptr<Scorer>> v;
    for (const auto& [f, t] : fterms)
        v.push_back(std::make_unique<TermScorer>(f, t, ctx));
    return v;
}

// 从 SearchResult 向量提取 doc_id 集合
static std::set<DocId> toIdSet(const std::vector<SearchResult>& rs) {
    std::set<DocId> s;
    for (const auto& r : rs) s.insert(r.doc_id);
    return s;
}

// ── 测试 1：Top-K 召回正确性 ──────────────────────────────────────────────────

static void test_topk_correctness() {
    TEST("WANDScorer Top-K 召回正确性");
    auto dir = tmpDir("topk");
    {
        IndexWriter w(dir, 16);
        for (int i = 1; i <= 100; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 3 == 0) body += " machine";
            if (i % 5 == 0) body += " learn";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string, std::string>> fterms = {
        {"body", stem("python")},
        {"body", stem("machine")},
        {"body", stem("learn")},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    int top_k = 5;
    WANDScorer ws(makeTermScorers(fterms, ctx), top_k, ctx);
    auto got = toIdSet(ws.collectTopK());

    // 必须恰好返回 top_k 个文档
    if ((int)got.size() != top_k) FAIL("Top-K 结果数量不对");

    // 与暴力基准比较（允许同分时集合相同即可）
    auto expected = bruteForceTopKIds(seg, ctx, fterms, top_k);
    if (got != expected) FAIL("Top-K 结果与暴力枚举不一致");

    PASS();
    fs::remove_all(dir);
}

// ── 测试 2：空游标（term 不存在）────────────────────────────────────────────

static void test_empty_cursors() {
    TEST("WANDScorer 空游标（term 不存在于索引）");
    auto dir = tmpDir("empty");
    {
        IndexWriter w(dir, 8);
        Document doc; doc.doc_id = 1; doc.set("body", "python");
        w.addDocument(doc);
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string, std::string>> fterms = {
        {"body", "nonexistent_term_xyz"},
    };
    TermIdfs idfs;
    ScorerContext ctx{seg, idfs, total, nullptr};

    WANDScorer ws(makeTermScorers(fterms, ctx), 10, ctx);
    auto results = ws.collectTopK();
    if (!results.empty()) FAIL("空游标时应返回空结果");
    if (!ws.isEnd()) FAIL("空游标时 isEnd() 应为 true");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 3：top_k >= 总命中数 → 返回所有命中 ─────────────────────────────────

static void test_topk_ge_total() {
    TEST("WANDScorer top_k >= 总命中数时返回所有命中");
    auto dir = tmpDir("all");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 10; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "deep";
            if (i % 2 == 0) body += " learn";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string, std::string>> fterms = {
        {"body", stem("deep")},
        {"body", stem("learn")},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    // 所有 10 个 doc 至少含 "deep"，top_k=100 应全部返回
    WANDScorer ws(makeTermScorers(fterms, ctx), 100, ctx);
    auto results = ws.collectTopK();
    if ((int)results.size() != 10) FAIL("top_k=100 时应返回全部 10 个命中");

    // 与暴力基准比较
    auto expected = bruteForceTopKIds(seg, ctx, fterms, 100);
    if (toIdSet(results) != expected) FAIL("全量返回时结果与暴力基准不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 4：单子节点退化 ─────────────────────────────────────────────────────

static void test_single_cursor() {
    TEST("WANDScorer 单子节点退化（一个 TermScorer）");
    auto dir = tmpDir("single");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 20; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            // 每隔 3 个含 "neural"
            if (i % 3 == 0) doc.set("body", "neural network");
            else             doc.set("body", "other topic");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t = stem("neural");
    TermIdfs idfs;
    const TermMeta* m = seg.getTermMeta("body", t);
    if (!m) FAIL("term 应存在");
    idfs["body:" + t] = calcIdf(total, m->doc_freq);
    ScorerContext ctx{seg, idfs, total, nullptr};

    // 单 cursor，top_k=3
    int top_k = 3;
    std::vector<std::unique_ptr<Scorer>> v;
    v.push_back(std::make_unique<TermScorer>("body", t, ctx));
    WANDScorer ws(std::move(v), top_k, ctx);
    auto got = toIdSet(ws.collectTopK());

    auto expected = bruteForceTopKIds(seg, ctx, {{"body", t}}, top_k);
    if (got != expected) FAIL("单子节点 Top-K 与暴力枚举不一致");
    if ((int)got.size() != top_k) FAIL("应恰好返回 top_k 个结果");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 5：跨 Block 边界（>128 doc），BlockMax 跳跃后结果仍正确 ───────────

static void test_cross_block() {
    TEST("WANDScorer 跨 Block 边界（300 doc），BlockMax 后召回正确");
    auto dir = tmpDir("block");
    {
        IndexWriter w(dir, 32);
        for (int i = 1; i <= 300; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            // 少数 doc 同时含 "rare"，得分更高
            if (i % 50 == 0) body += " rare";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string, std::string>> fterms = {
        {"body", stem("python")},
        {"body", "rare"},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    int top_k = 5;
    WANDScorer ws(makeTermScorers(fterms, ctx), top_k, ctx);
    auto got = toIdSet(ws.collectTopK());
    auto expected = bruteForceTopKIds(seg, ctx, fterms, top_k);

    if (got != expected) FAIL("跨 Block 边界后 Top-K 召回与暴力基准不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 6：maxScore() = Σ children maxScores ─────────────────────────────

static void test_max_score() {
    TEST("WANDScorer maxScore() = Σ children maxScores");
    auto dir = tmpDir("maxs");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 20; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body", "deep learning neural");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t1 = stem("deep"), t2 = stem("learning"), t3 = stem("neural");
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
    WANDScorer ws(std::move(v), 5, ctx);

    if (std::abs(ws.maxScore() - expected) > 1e-5f)
        FAIL("maxScore() 不等于 Σ children maxScores");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 7：Scorer 接口 next() / docId() / isEnd() 顺序迭代 ──────────────

static void test_scorer_interface_next() {
    TEST("WANDScorer Scorer 接口 next()/docId()/isEnd() 顺序迭代");
    auto dir = tmpDir("next");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 30; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 2 == 0) body += " java";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string, std::string>> fterms = {
        {"body", stem("python")},
        {"body", "java"},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    int top_k = 8;
    // 先通过 collectTopK 获取参考集
    WANDScorer ws_ref(makeTermScorers(fterms, ctx), top_k, ctx);
    auto ref_ids = toIdSet(ws_ref.collectTopK());

    // 再用 Scorer 接口迭代（doc_id 升序）
    WANDScorer ws(makeTermScorers(fterms, ctx), top_k, ctx);
    std::vector<DocId> got_ids;
    bool ascending = true;
    DocId prev = 0;
    while (!ws.isEnd()) {
        DocId d = ws.docId();
        got_ids.push_back(d);
        if (d < prev) ascending = false;
        prev = d;
        ws.next();
    }

    if (!ascending) FAIL("Scorer 接口迭代应按 doc_id 升序");
    if (std::set<DocId>(got_ids.begin(), got_ids.end()) != ref_ids)
        FAIL("Scorer 接口遍历的 doc_id 集合与 collectTopK 不一致");

    // 耗尽后继续 next() 应保持 isEnd()==true
    ws.next();
    ws.next();
    if (!ws.isEnd()) FAIL("耗尽后 isEnd() 应保持 true");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 8：Scorer 接口 advance() 跳跃语义 ───────────────────────────────

static void test_scorer_interface_advance() {
    TEST("WANDScorer Scorer 接口 advance() 跳跃语义");
    auto dir = tmpDir("adv");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 50; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 4 == 0) body += " scala";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string, std::string>> fterms = {
        {"body", stem("python")},
        {"body", "scala"},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    int top_k = 10;
    WANDScorer ws(makeTermScorers(fterms, ctx), top_k, ctx);

    // 跳到第一个 doc_id >= 20 的结果
    DocId target = 20;
    bool found = ws.advance(target);

    // 验证：advance() 落点 >= target
    if (found && ws.docId() < target)
        FAIL("advance() 后 docId() < target");

    // advance 到不存在的位置：超过所有 doc_id
    bool found2 = ws.advance(INVALID_DOC - 1);
    if (found2) FAIL("advance 到超大 target 应返回 false");
    if (!ws.isEnd()) FAIL("advance 到尽头后 isEnd() 应为 true");

    PASS();
    fs::remove_all(dir);
}

// ── 测试 9：top_k = 0 → 空结果（边界异常情形）────────────────────────────

static void test_topk_zero() {
    TEST("WANDScorer top_k = 0 返回空结果");
    auto dir = tmpDir("zero");
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
    const TermMeta* m = seg.getTermMeta("body", t);
    if (m) idfs["body:" + t] = calcIdf(total, m->doc_freq);
    ScorerContext ctx{seg, idfs, total, nullptr};

    std::vector<std::unique_ptr<Scorer>> v;
    v.push_back(std::make_unique<TermScorer>("body", t, ctx));
    WANDScorer ws(std::move(v), 0, ctx);  // top_k = 0

    auto results = ws.collectTopK();
    if (!results.empty()) FAIL("top_k=0 时应返回空结果");
    if (!ws.isEnd()) FAIL("top_k=0 时 Scorer 接口应 isEnd()==true");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 10：collectTopK() 结果按得分降序排列 ────────────────────────────────

static void test_result_score_order() {
    TEST("WANDScorer collectTopK() 结果按得分降序排列");
    auto dir = tmpDir("order");
    {
        IndexWriter w(dir, 8);
        // 让不同 doc 的得分有差异：高频词 "common" 出现在所有 doc，
        // "rare" 只出现在少数 doc（IDF 更高 → 得分更高）
        for (int i = 1; i <= 30; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "common";
            if (i % 10 == 0) body += " rare";    // doc 10,20,30 含 "rare"
            if (i % 7  == 0) body += " special";  // doc 7,14,21,28
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string, std::string>> fterms = {
        {"body", stem("common")},
        {"body", "rare"},
        {"body", "special"},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr};

    WANDScorer ws(makeTermScorers(fterms, ctx), 10, ctx);
    auto results = ws.collectTopK();

    if ((int)results.size() != 10) FAIL("top_k=10 时应返回 10 条结果");

    // 验证得分降序
    for (size_t i = 1; i < results.size(); ++i) {
        if (results[i].score > results[i - 1].score + 1e-6f)
            FAIL("collectTopK() 结果未按得分降序排列");
    }

    // 验证高得分文档必然出现在结果中（rare=docs 10,20,30; special=docs 7,14,21,28）
    // 这 7 个文档得分明确高于 common-only 文档，必须全部出现在 top-10 中
    std::set<DocId> got_ids = toIdSet(results);
    for (DocId must : {10u, 20u, 30u, 7u, 14u, 21u, 28u}) {
        if (!got_ids.count(must))
            FAIL("高得分文档未出现在 top-10 结果中");
    }

    // 结果中所有文档的得分均 >= 最低得分（由结果集本身验证）
    float min_score = results.back().score;
    for (const auto& r : results) {
        if (r.score < min_score - 1e-6f) FAIL("存在得分低于最小值的结果");
    }

    PASS();
    fs::remove_all(dir);
}

// ── 入口 ─────────────────────────────────────────────────────────────────────

int main() {
    test_topk_correctness();
    test_empty_cursors();
    test_topk_ge_total();
    test_single_cursor();
    test_cross_block();
    test_max_score();
    test_scorer_interface_next();
    test_scorer_interface_advance();
    test_topk_zero();
    test_result_score_order();

    std::cout << "\n═══════════════════════════════════\n";
    std::cout << "  All WANDScorer tests PASSED!\n";
    std::cout << "═══════════════════════════════════\n";
    return 0;
}
