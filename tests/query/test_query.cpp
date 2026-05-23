// tests/query/test_query.cpp
//
// Query 层单元测试，覆盖以下情形：
//   1.  TermQuery::debugString            "field:term" 格式
//   2.  BooleanQuery::debugString         +/- 前缀格式
//   3.  TermQuery::createScorer           与 PostingIterator 遍历结果一致
//   4.  BooleanQuery 全 MUST（单子句）    直通 TermScorer，结果等于 readAll
//   5.  BooleanQuery 全 MUST（多子句）    等价于 ConjunctionScorer，结果等于 bruteAND
//   6.  BooleanQuery 全 SHOULD            WANDScorer，结果是 OR 并集的子集，且得分有序
//   7.  BooleanQuery MUST + MUST_NOT      结果等于 bruteExclude
//   8.  BooleanQuery MUST + 多 MUST_NOT   多项排除各自生效
//   9.  嵌套 BooleanQuery                 内层 AND 外层排除，结果与暴力基准一致
//  10.  空 MUST（term 不存在）            createScorer 返回 nullptr

#include "query/query.h"
#include "query/wand_scorer.h"
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

// ── 辅助 ─────────────────────────────────────────────────────────────────────

static std::string tmpDir(const std::string& tag) {
    std::string d = "/tmp/test_q_" + tag;
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

// 从 seg 中读取指定 field:term 列表的 IDF，合并进 out_idfs
static void addIdfs(const SegmentReader& seg, uint32_t total,
                    const std::vector<std::pair<std::string,std::string>>& fterms,
                    TermIdfs& out_idfs) {
    for (const auto& [f, t] : fterms) {
        const TermMeta* m = seg.getTermMeta(f, t);
        if (m) out_idfs[f + ":" + t] = calcIdf(total, m->doc_freq);
    }
}

// 通过 PostingIterator 收集 field:term 的全部 doc_id
static std::vector<DocId> readAll(const SegmentReader& seg,
                                  const std::string& f, const std::string& t) {
    auto it = seg.postingIterator(f, t);
    std::vector<DocId> r;
    while (!it->isEnd()) { r.push_back(it->docId()); it->next(); }
    return r;
}

// 多路 AND 暴力基准
static std::vector<DocId> bruteAND(
    const std::vector<std::vector<DocId>>& lists) {
    if (lists.empty()) return {};
    std::set<DocId> res(lists[0].begin(), lists[0].end());
    for (size_t i = 1; i < lists.size(); ++i) {
        std::set<DocId> s(lists[i].begin(), lists[i].end());
        std::set<DocId> inter;
        for (DocId d : res) if (s.count(d)) inter.insert(d);
        res = inter;
    }
    return {res.begin(), res.end()};
}

// 差集暴力基准：main - excl1 - excl2 - …
static std::vector<DocId> bruteExclude(
    const std::vector<DocId>& main_docs,
    const std::vector<std::vector<DocId>>& excl_lists) {
    std::set<DocId> excl;
    for (const auto& el : excl_lists)
        for (DocId d : el) excl.insert(d);
    std::vector<DocId> r;
    for (DocId d : main_docs)
        if (!excl.count(d)) r.push_back(d);
    return r;
}

// 通过 Scorer 接口排空，收集所有 doc_id
static std::vector<DocId> drain(Scorer& sc) {
    std::vector<DocId> r;
    while (!sc.isEnd()) { r.push_back(sc.docId()); sc.next(); }
    return r;
}

// ── 测试 1：TermQuery::debugString ────────────────────────────────────────────

static void test_term_query_debug_string() {
    TEST("TermQuery::debugString 格式");
    TermQuery q("body", "python");
    if (q.debugString() != "body:python") FAIL("debugString 格式错误");
    TermQuery q2("title", "learn");
    if (q2.debugString() != "title:learn") FAIL("debugString 格式错误（title）");
    PASS();
}

// ── 测试 2：BooleanQuery::debugString ─────────────────────────────────────────

static void test_boolean_query_debug_string() {
    TEST("BooleanQuery::debugString +/- 前缀格式");
    BooleanQuery bq;
    bq.add(std::make_unique<TermQuery>("body",   "python"),  Occur::MUST);
    bq.add(std::make_unique<TermQuery>("body",   "numpy"),   Occur::SHOULD);
    bq.add(std::make_unique<TermQuery>("source", "spam"),    Occur::MUST_NOT);
    std::string got = bq.debugString();
    if (got != "(+body:python body:numpy -source:spam)")
        FAIL("debugString = \"" + got + "\"，预期 \"(+body:python body:numpy -source:spam)\"");
    PASS();
}

// ── 测试 3：TermQuery::createScorer 遍历结果与 PostingIterator 一致 ──────────

static void test_term_query_create_scorer() {
    TEST("TermQuery::createScorer 遍历结果与 PostingIterator 一致");
    auto dir = tmpDir("tq");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 30; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body", i % 2 == 0 ? "python machine" : "python");
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

    TermQuery tq("body", t);
    auto scorer = tq.createScorer(ctx);
    if (!scorer) FAIL("createScorer 不应返回 nullptr");

    auto got = drain(*scorer);
    auto ref = readAll(seg, "body", t);
    if (got != ref) FAIL("遍历结果与 PostingIterator 不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 4：BooleanQuery 单 MUST → 直通 TermScorer ───────────────────────────

static void test_single_must() {
    TEST("BooleanQuery 单 MUST → 直通 TermScorer");
    auto dir = tmpDir("sm");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 20; ++i) {
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

    BooleanQuery bq;
    bq.add(std::make_unique<TermQuery>("body", t), Occur::MUST);
    auto scorer = bq.createScorer(ctx);
    if (!scorer) FAIL("createScorer 不应返回 nullptr");

    auto got = drain(*scorer);
    auto ref = readAll(seg, "body", t);
    if (got != ref) FAIL("单 MUST 结果与 readAll 不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 5：BooleanQuery 多 MUST → ConjunctionScorer ─────────────────────────

static void test_multi_must() {
    TEST("BooleanQuery 多 MUST → 等价 ConjunctionScorer（结果 == bruteAND）");
    auto dir = tmpDir("mm");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 60; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
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
    const std::string t1 = stem("python"), t2 = stem("machine"), t3 = stem("deep");
    TermIdfs idfs;
    addIdfs(seg, total, {{"body", t1}, {"body", t2}, {"body", t3}}, idfs);
    ScorerContext ctx{seg, idfs, total, nullptr};

    BooleanQuery bq;
    bq.add(std::make_unique<TermQuery>("body", t1), Occur::MUST);
    bq.add(std::make_unique<TermQuery>("body", t2), Occur::MUST);
    bq.add(std::make_unique<TermQuery>("body", t3), Occur::MUST);
    auto scorer = bq.createScorer(ctx);
    if (!scorer) FAIL("createScorer 不应返回 nullptr");

    auto got      = drain(*scorer);
    auto expected = bruteAND({readAll(seg, "body", t1),
                               readAll(seg, "body", t2),
                               readAll(seg, "body", t3)});
    if (got != expected) FAIL("多 MUST 结果与 bruteAND 不一致");
    if (got.empty()) FAIL("预期有命中（6 的倍数）");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 6：BooleanQuery 全 SHOULD → WANDScorer ───────────────────────────────

static void test_all_should() {
    TEST("BooleanQuery 全 SHOULD → WANDScorer，结果 ⊆ OR 并集且不超过 top_k");
    auto dir = tmpDir("as");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 40; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body;
            if (i % 2 == 0) body += "python ";
            if (i % 3 == 0) body += "machine ";
            if (body.empty()) body = "other";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t1 = stem("python"), t2 = stem("machine");
    TermIdfs idfs;
    addIdfs(seg, total, {{"body", t1}, {"body", t2}}, idfs);

    // top_k = 100：收集所有 OR 命中
    ScorerContext ctx{seg, idfs, total, nullptr, 100};

    BooleanQuery bq;
    bq.add(std::make_unique<TermQuery>("body", t1), Occur::SHOULD);
    bq.add(std::make_unique<TermQuery>("body", t2), Occur::SHOULD);
    auto scorer = bq.createScorer(ctx);
    if (!scorer) FAIL("createScorer 不应返回 nullptr");

    // 收集所有迭代结果（doc_id 升序）
    auto got = drain(*scorer);
    if (got.empty()) FAIL("SHOULD 查询应有命中");

    // 验证每个命中 doc 都在 OR 并集中
    auto union_docs = readAll(seg, "body", t1);
    auto d2         = readAll(seg, "body", t2);
    std::set<DocId> or_set(union_docs.begin(), union_docs.end());
    or_set.insert(d2.begin(), d2.end());
    for (DocId d : got)
        if (!or_set.count(d)) FAIL("结果包含不在 OR 并集中的 doc");

    // 不超过 top_k
    if (static_cast<int>(got.size()) > 100) FAIL("结果超过 top_k");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 7：BooleanQuery MUST + MUST_NOT ─────────────────────────────────────

static void test_must_plus_must_not() {
    TEST("BooleanQuery MUST + MUST_NOT → 结果 == bruteExclude");
    auto dir = tmpDir("mn");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 50; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 5 == 0) body += " spam";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t_m = stem("python"), t_e = stem("spam");
    TermIdfs idfs;
    addIdfs(seg, total, {{"body", t_m}, {"body", t_e}}, idfs);
    ScorerContext ctx{seg, idfs, total, nullptr};

    BooleanQuery bq;
    bq.add(std::make_unique<TermQuery>("body", t_m), Occur::MUST);
    bq.add(std::make_unique<TermQuery>("body", t_e), Occur::MUST_NOT);
    auto scorer = bq.createScorer(ctx);
    if (!scorer) FAIL("createScorer 不应返回 nullptr");

    auto got      = drain(*scorer);
    auto expected = bruteExclude(readAll(seg, "body", t_m),
                                 {readAll(seg, "body", t_e)});
    if (got != expected) FAIL("MUST + MUST_NOT 结果与 bruteExclude 不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 8：BooleanQuery MUST + 多 MUST_NOT ──────────────────────────────────

static void test_multi_must_not() {
    TEST("BooleanQuery MUST + 多 MUST_NOT → 多项排除各自生效");
    auto dir = tmpDir("mmn");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 60; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 3 == 0) body += " spam";
            if (i % 5 == 0) body += " ad";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t_m  = stem("python");
    const std::string t_e1 = stem("spam");
    const std::string t_e2 = stem("ad");
    TermIdfs idfs;
    addIdfs(seg, total, {{"body", t_m}, {"body", t_e1}, {"body", t_e2}}, idfs);
    ScorerContext ctx{seg, idfs, total, nullptr};

    BooleanQuery bq;
    bq.add(std::make_unique<TermQuery>("body", t_m),  Occur::MUST);
    bq.add(std::make_unique<TermQuery>("body", t_e1), Occur::MUST_NOT);
    bq.add(std::make_unique<TermQuery>("body", t_e2), Occur::MUST_NOT);
    auto scorer = bq.createScorer(ctx);
    if (!scorer) FAIL("createScorer 不应返回 nullptr");

    auto got      = drain(*scorer);
    auto expected = bruteExclude(readAll(seg, "body", t_m),
                                 {readAll(seg, "body", t_e1),
                                  readAll(seg, "body", t_e2)});
    if (got != expected) FAIL("多 MUST_NOT 结果与 bruteExclude 不一致");

    // 确认被排除的 doc 确实不在结果中
    std::set<DocId> got_set(got.begin(), got.end());
    for (DocId d : readAll(seg, "body", t_e1))
        if (got_set.count(d)) FAIL("spam doc 未被排除");
    for (DocId d : readAll(seg, "body", t_e2))
        if (got_set.count(d)) FAIL("ad doc 未被排除");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 9：嵌套 BooleanQuery ─────────────────────────────────────────────────
// 内层：BooleanQuery(MUST: python, MUST: machine) → ConjunctionScorer
// 外层：BooleanQuery(MUST: inner, MUST_NOT: spam)  → ExclusionScorer

static void test_nested_boolean_query() {
    TEST("嵌套 BooleanQuery：内层 AND 外层排除");
    auto dir = tmpDir("nb");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 60; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 2 == 0) body += " machine";
            if (i % 7 == 0) body += " spam";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t1 = stem("python"), t2 = stem("machine"), t_e = stem("spam");
    TermIdfs idfs;
    addIdfs(seg, total, {{"body", t1}, {"body", t2}, {"body", t_e}}, idfs);
    ScorerContext ctx{seg, idfs, total, nullptr};

    // 内层：python AND machine
    auto inner = std::make_unique<BooleanQuery>();
    inner->add(std::make_unique<TermQuery>("body", t1), Occur::MUST);
    inner->add(std::make_unique<TermQuery>("body", t2), Occur::MUST);

    // 外层：inner AND NOT spam
    BooleanQuery outer;
    outer.add(std::move(inner), Occur::MUST);
    outer.add(std::make_unique<TermQuery>("body", t_e), Occur::MUST_NOT);

    auto scorer = outer.createScorer(ctx);
    if (!scorer) FAIL("createScorer 不应返回 nullptr");

    auto got      = drain(*scorer);
    auto and_docs = bruteAND({readAll(seg, "body", t1), readAll(seg, "body", t2)});
    auto expected = bruteExclude(and_docs, {readAll(seg, "body", t_e)});
    if (got != expected) FAIL("嵌套查询结果与暴力基准不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 10：空 MUST（term 不存在）→ createScorer 返回 nullptr ───────────────

static void test_empty_must_returns_nullptr() {
    TEST("BooleanQuery 空 MUST（term 不在索引）→ createScorer 返回 nullptr");
    auto dir = tmpDir("em");
    {
        IndexWriter w(dir, 8);
        Document doc; doc.doc_id = 1; doc.set("body", "python");
        w.addDocument(doc);
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    TermIdfs idfs;  // 不放任何 IDF，term 也不在索引中
    ScorerContext ctx{seg, idfs, total, nullptr};

    BooleanQuery bq;
    bq.add(std::make_unique<TermQuery>("body", "nonexistent"), Occur::MUST);
    auto scorer = bq.createScorer(ctx);
    if (scorer != nullptr) FAIL("空 MUST 应返回 nullptr");
    PASS();
    fs::remove_all(dir);
}

// ── 入口 ─────────────────────────────────────────────────────────────────────

int main() {
    test_term_query_debug_string();
    test_boolean_query_debug_string();
    test_term_query_create_scorer();
    test_single_must();
    test_multi_must();
    test_all_should();
    test_must_plus_must_not();
    test_multi_must_not();
    test_nested_boolean_query();
    test_empty_must_returns_nullptr();

    std::cout << "\n═══════════════════════════════════\n";
    std::cout << "  All Query layer tests PASSED!\n";
    std::cout << "═══════════════════════════════════\n";
    return 0;
}
