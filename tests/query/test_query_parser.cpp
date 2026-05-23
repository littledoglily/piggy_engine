// tests/query/test_query_parser.cpp
//
// QueryParser 单元测试，覆盖以下情形：
//   1.  纯 SHOULD：裸词展开到所有默认字段（debugString 验证）
//   2.  MUST + MUST_NOT field:term（debugString 验证）
//   3.  +/- 裸词：MUST/MUST_NOT 裸 term 包裹为内层 BooleanQuery
//   4.  AND 模式（default_occur=MUST）：裸词包裹为内层 BooleanQuery
//   5.  无前缀 field:term 走 default_occur（SHOULD）
//   6.  混合：bare term + field:term + +/- 前缀
//   7.  功能验证：SHOULD 查询 → 结果 ⊆ OR 并集
//   8.  功能验证：MUST + MUST_NOT → 结果 == bruteExclude
//   9.  功能验证：AND 模式裸词 → 结果 == bruteAND（跨字段 MUST）
//  10.  停用词 / 空查询 → createScorer 返回 nullptr

#include "query/query_parser.h"
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

static std::string tmpDir(const std::string& tag) {
    std::string d = "/tmp/test_qp_" + tag;
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

static void addIdf(TermIdfs& idfs, const SegmentReader& seg,
                   uint32_t total, const std::string& field, const std::string& term) {
    const TermMeta* m = seg.getTermMeta(field, term);
    if (m) idfs[field + ":" + term] = calcIdf(total, m->doc_freq);
}

static std::vector<DocId> readAll(const SegmentReader& seg,
                                  const std::string& f, const std::string& t) {
    auto it = seg.postingIterator(f, t);
    std::vector<DocId> r;
    while (!it->isEnd()) { r.push_back(it->docId()); it->next(); }
    return r;
}

static std::vector<DocId> bruteAND(const std::vector<std::vector<DocId>>& lists) {
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

static std::vector<DocId> bruteExclude(const std::vector<DocId>& main_docs,
                                        const std::vector<std::vector<DocId>>& excl) {
    std::set<DocId> ex;
    for (const auto& el : excl) for (DocId d : el) ex.insert(d);
    std::vector<DocId> r;
    for (DocId d : main_docs) if (!ex.count(d)) r.push_back(d);
    return r;
}

static std::vector<DocId> drain(Scorer& sc) {
    std::vector<DocId> r;
    while (!sc.isEnd()) { r.push_back(sc.docId()); sc.next(); }
    return r;
}

// ── 测试 1：SHOULD 裸词展开到所有默认字段 ───────────────────────────────────

static void test_should_bare_term_debug_string() {
    TEST("QueryParser SHOULD 裸词展开 debugString");
    Analyzer a;
    QueryParser parser(a, {"body", "title"});
    const std::string t = stem("python");
    auto q = parser.parse("python");
    // 期望：body:t SHOULD + title:t SHOULD → "(body:t title:t)"
    std::string expected = "(" + std::string("body:") + t + " title:" + t + ")";
    if (q->debugString() != expected)
        FAIL("debugString = \"" + q->debugString() + "\"，预期 \"" + expected + "\"");
    PASS();
}

// ── 测试 2：MUST + MUST_NOT field:term ───────────────────────────────────────

static void test_must_must_not_field_term_debug_string() {
    TEST("QueryParser +field:term -field:term debugString");
    Analyzer a;
    QueryParser parser(a, {"body"});
    const std::string tp = stem("python");
    const std::string ts = stem("spam");
    auto q = parser.parse("+body:python -body:spam");
    std::string expected = "(+body:" + tp + " -body:" + ts + ")";
    if (q->debugString() != expected)
        FAIL("debugString = \"" + q->debugString() + "\"，预期 \"" + expected + "\"");
    PASS();
}

// ── 测试 3：+/- 裸词包裹为内层 BooleanQuery ──────────────────────────────────

static void test_bare_term_with_prefix_debug_string() {
    TEST("QueryParser +/- 裸词包裹为内层 BooleanQuery");
    Analyzer a;
    QueryParser parser(a, {"body", "title"});
    const std::string tp = stem("python");
    const std::string ts = stem("spam");
    // "+python -spam" → MUST inner(body:tp, title:tp), MUST_NOT inner(body:ts, title:ts)
    auto q = parser.parse("+python -spam");
    std::string inner_p = "(body:" + tp + " title:" + tp + ")";
    std::string inner_s = "(body:" + ts + " title:" + ts + ")";
    std::string expected = "(+" + inner_p + " -" + inner_s + ")";
    if (q->debugString() != expected)
        FAIL("debugString = \"" + q->debugString() + "\"，预期 \"" + expected + "\"");
    PASS();
}

// ── 测试 4：AND 模式裸词包裹为内层 BooleanQuery ──────────────────────────────

static void test_and_mode_bare_term_debug_string() {
    TEST("QueryParser AND 模式裸词包裹为内层 BooleanQuery");
    Analyzer a;
    QueryParser parser(a, {"body", "title"});
    const std::string t1 = stem("python");
    const std::string t2 = stem("machine");
    auto q = parser.parse("python machine", Occur::MUST);
    std::string inner1 = "(body:" + t1 + " title:" + t1 + ")";
    std::string inner2 = "(body:" + t2 + " title:" + t2 + ")";
    std::string expected = "(+" + inner1 + " +" + inner2 + ")";
    if (q->debugString() != expected)
        FAIL("debugString = \"" + q->debugString() + "\"，预期 \"" + expected + "\"");
    PASS();
}

// ── 测试 5：无前缀 field:term 走 default_occur ───────────────────────────────

static void test_fieldterm_default_occur() {
    TEST("QueryParser 无前缀 field:term 走 default_occur");
    Analyzer a;
    QueryParser parser(a, {"body"});
    const std::string tp = stem("python");
    // default SHOULD
    auto qs = parser.parse("body:python");
    if (qs->debugString() != "(body:" + tp + ")")
        FAIL("SHOULD: " + qs->debugString());
    // AND 模式 → MUST
    auto qm = parser.parse("body:python", Occur::MUST);
    if (qm->debugString() != "(+body:" + tp + ")")
        FAIL("MUST: " + qm->debugString());
    PASS();
}

// ── 测试 6：混合 bare term + field:term + 前缀 ───────────────────────────────

static void test_mixed_query_debug_string() {
    TEST("QueryParser 混合查询 debugString");
    Analyzer a;
    QueryParser parser(a, {"body"});
    const std::string tp = stem("python");
    const std::string ts = stem("spam");
    const std::string tn = stem("numpy");
    // "+python body:numpy -source:spam"
    // +python → MUST inner(body:tp)，body:numpy → SHOULD body:tn，-source:spam → MUST_NOT source:ts
    auto q = parser.parse("+python body:numpy -source:spam");
    std::string inner_p = "(body:" + tp + ")";
    std::string expected = "(+" + inner_p + " body:" + tn + " -source:" + ts + ")";
    if (q->debugString() != expected)
        FAIL("debugString = \"" + q->debugString() + "\"，预期 \"" + expected + "\"");
    PASS();
}

// ── 测试 7：功能验证 - SHOULD 查询结果 ⊆ OR 并集 ────────────────────────────

static void test_functional_should() {
    TEST("QueryParser SHOULD 功能验证：结果 ⊆ OR 并集");
    auto dir = tmpDir("sh");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 40; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body", i % 2 == 0 ? "python" : "machine");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t1 = stem("python"), t2 = stem("machine");
    TermIdfs idfs;
    addIdf(idfs, seg, total, "body", t1);
    addIdf(idfs, seg, total, "body", t2);
    ScorerContext ctx{seg, idfs, total, nullptr, 100};

    Analyzer a;
    QueryParser parser(a, {"body"});
    auto q = parser.parse("python machine");  // 全 SHOULD
    auto scorer = q->createScorer(ctx);
    if (!scorer) FAIL("createScorer 不应返回 nullptr");

    auto got = drain(*scorer);
    if (got.empty()) FAIL("SHOULD 查询应有命中");

    std::set<DocId> or_set;
    for (DocId d : readAll(seg, "body", t1)) or_set.insert(d);
    for (DocId d : readAll(seg, "body", t2)) or_set.insert(d);
    for (DocId d : got)
        if (!or_set.count(d)) FAIL("结果包含不在 OR 并集中的 doc");

    PASS();
    fs::remove_all(dir);
}

// ── 测试 8：功能验证 - MUST + MUST_NOT ──────────────────────────────────────

static void test_functional_must_must_not() {
    TEST("QueryParser MUST + MUST_NOT 功能验证：结果 == bruteExclude");
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
    const std::string tp = stem("python"), ts = stem("spam");
    TermIdfs idfs;
    addIdf(idfs, seg, total, "body", tp);
    addIdf(idfs, seg, total, "body", ts);
    ScorerContext ctx{seg, idfs, total, nullptr};

    Analyzer a;
    QueryParser parser(a, {"body"});
    auto q = parser.parse("+body:python -body:spam");
    auto scorer = q->createScorer(ctx);
    if (!scorer) FAIL("createScorer 不应返回 nullptr");

    auto got      = drain(*scorer);
    auto expected = bruteExclude(readAll(seg, "body", tp), {readAll(seg, "body", ts)});
    if (got != expected) FAIL("结果与 bruteExclude 不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 9：功能验证 - AND 模式裸词跨字段 MUST ──────────────────────────────
// 索引两个字段 body / title；要求 doc 在任意字段命中 "python" AND 任意字段命中 "deep"

static void test_functional_and_mode_bare_term() {
    TEST("QueryParser AND 模式裸词跨字段 MUST：结果等于暴力 AND（任意字段命中）");
    auto dir = tmpDir("am");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 60; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            // body 有 python（偶数）；title 有 deep（3 的倍数）
            doc.set("body",  i % 2 == 0 ? "python" : "other");
            doc.set("title", i % 3 == 0 ? "deep"   : "nothing");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string tp = stem("python"), td = stem("deep");
    TermIdfs idfs;
    addIdf(idfs, seg, total, "body",  tp);
    addIdf(idfs, seg, total, "body",  td);
    addIdf(idfs, seg, total, "title", tp);
    addIdf(idfs, seg, total, "title", td);
    // top_k 足够大，使内层 WANDScorer 返回全部命中，ConjunctionScorer 才能正确求交
    ScorerContext ctx{seg, idfs, total, nullptr, 100};

    Analyzer a;
    QueryParser parser(a, {"body", "title"});
    // AND 模式："python deep" →
    //   MUST(body:python SHOULD title:python) AND MUST(body:deep SHOULD title:deep)
    auto q = parser.parse("python deep", Occur::MUST);
    auto scorer = q->createScorer(ctx);
    if (!scorer) FAIL("createScorer 不应返回 nullptr");

    auto got = drain(*scorer);
    if (got.empty()) FAIL("预期有命中（偶数且 3 的倍数）");

    // 暴力基准：(body:python OR title:python) AND (body:deep OR title:deep)
    std::set<DocId> has_python, has_deep;
    for (DocId d : readAll(seg, "body",  tp)) has_python.insert(d);
    for (DocId d : readAll(seg, "title", tp)) has_python.insert(d);
    for (DocId d : readAll(seg, "body",  td)) has_deep.insert(d);
    for (DocId d : readAll(seg, "title", td)) has_deep.insert(d);

    std::set<DocId> expected_set;
    for (DocId d : has_python) if (has_deep.count(d)) expected_set.insert(d);
    std::vector<DocId> expected(expected_set.begin(), expected_set.end());

    if (got != expected) FAIL("结果与暴力 AND（任意字段）不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 10：停用词 / 空查询 → createScorer 返回 nullptr ─────────────────────

static void test_empty_or_stopword_query() {
    TEST("QueryParser 停用词 / 空查询 → createScorer 返回 nullptr");
    auto dir = tmpDir("eq");
    {
        IndexWriter w(dir, 8);
        Document doc; doc.doc_id = 1; doc.set("body", "python");
        w.addDocument(doc); w.flush();
    }
    SegmentReader seg(dir, 0);
    TermIdfs idfs;
    ScorerContext ctx{seg, idfs, seg.docCount(), nullptr};

    Analyzer a;
    QueryParser parser(a, {"body"});

    // 空字符串
    auto q1 = parser.parse("");
    auto s1 = q1->createScorer(ctx);
    if (s1 != nullptr) FAIL("空查询应返回 nullptr");

    // 纯停用词（"the" 是停用词）
    auto q2 = parser.parse("the");
    auto s2 = q2->createScorer(ctx);
    if (s2 != nullptr) FAIL("纯停用词查询应返回 nullptr");

    PASS();
    fs::remove_all(dir);
}

// ── 入口 ─────────────────────────────────────────────────────────────────────

int main() {
    test_should_bare_term_debug_string();
    test_must_must_not_field_term_debug_string();
    test_bare_term_with_prefix_debug_string();
    test_and_mode_bare_term_debug_string();
    test_fieldterm_default_occur();
    test_mixed_query_debug_string();
    test_functional_should();
    test_functional_must_must_not();
    test_functional_and_mode_bare_term();
    test_empty_or_stopword_query();

    std::cout << "\n═══════════════════════════════════\n";
    std::cout << "  All QueryParser tests PASSED!\n";
    std::cout << "═══════════════════════════════════\n";
    return 0;
}
