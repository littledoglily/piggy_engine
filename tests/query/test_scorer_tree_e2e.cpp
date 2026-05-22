// tests/query/test_scorer_tree_e2e.cpp
//
// Step 10: Scorer 树端到端综合测试
//
// 覆盖完整流水线：QueryParser → BooleanQuery → Scorer 树 → IndexSearcher
// 并补充 Merge、软删除、多 Segment 等场景。
//
//  1.  MUST + SHOULD + MUST_NOT 混合查询（通过 BooleanQuery 直接构造）
//  2.  嵌套 BooleanQuery：外层 MUST + 内层 SHOULD 子查询
//  3.  QueryParser → BooleanQuery → createScorer 结果与 IndexSearcher.search 一致
//  4.  AND 模式：多字段 bare term 要求所有字段命中
//  5.  OR 模式：任意字段命中即可，结果为 OR 并集子集
//  6.  Merge：两 Segment 合并后 search 结果与合并前相同
//  7.  软删除 + Merge：被删文档在合并后彻底消失
//  8.  多 Segment 搜索：两个 Segment 独立搜索结果汇总正确
//  9.  QueryParser + IndexSearcher：字段限定 vs 裸词展开行为一致
// 10.  空 Segment / 停用词查询 → 返回空结果（边界）

#include "query/index_searcher.h"
#include "query/query_parser.h"
#include "query/query.h"
#include "query/term_scorer.h"
#include "query/conjunction_scorer.h"
#include "query/wand_scorer.h"
#include "query/exclusion_scorer.h"
#include "query/scorer_context.h"
#include "index/segment_reader.h"
#include "index/segment_merger.h"
#include "index/index_writer.h"
#include "analysis/analyzer.h"
#include "../test_utils.h"

#include <filesystem>
#include <set>
#include <vector>
#include <cmath>
#include <algorithm>
#include <iostream>

using namespace ii;
namespace fs = std::filesystem;

// ── 辅助 ─────────────────────────────────────────────────────────────────────

static std::string tmpDir(const std::string& tag) {
    std::string d = "/tmp/test_e2e_" + tag;
    fs::remove_all(d);
    return d;
}

static std::string stem(const std::string& w) {
    Analyzer a;
    auto v = a.analyzeQuery(w);
    return v.empty() ? w : v[0];
}

using TermIdfs = std::unordered_map<std::string, float>;

static TermIdfs buildIdfs(const SegmentReader& seg, uint32_t total,
                          const std::vector<std::pair<std::string,std::string>>& fts) {
    TermIdfs m;
    for (const auto& [f, t] : fts) {
        const TermMeta* meta = seg.getTermMeta(f, t);
        if (!meta) continue;
        float idf = std::log(1.f + (float)(total - meta->doc_freq + 0.5f)
                                  / (float)(meta->doc_freq + 0.5f));
        m[f + ":" + t] = idf;
    }
    return m;
}

static std::vector<DocId> drainScorer(Scorer& sc) {
    std::vector<DocId> r;
    while (!sc.isEnd()) { r.push_back(sc.docId()); sc.next(); }
    return r;
}

static std::set<DocId> toIdSet(const std::vector<SearchResult>& rs) {
    std::set<DocId> s;
    for (const auto& r : rs) s.insert(r.doc_id);
    return s;
}

// ── 测试 1：MUST + SHOULD + MUST_NOT 混合（直接构造 BooleanQuery）────────────

static void test_must_should_must_not() {
    TEST("BooleanQuery MUST + SHOULD + MUST_NOT 混合");
    auto dir = tmpDir("msn");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 30; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";                       // 全部含 python
            if (i % 3 == 0) body += " numpy";                 // 3 的倍数含 numpy
            if (i % 5 == 0) body += " spam";                  // 5 的倍数含 spam（排除）
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string tp = stem("python"), tn = "numpy", ts = stem("spam");

    TermIdfs idfs = buildIdfs(seg, total,
        {{"body", tp}, {"body", tn}, {"body", ts}});
    ScorerContext ctx{seg, idfs, total, nullptr, 50};

    // +body:python body:numpy -body:spam
    BooleanQuery bq;
    bq.add(std::make_unique<TermQuery>("body", tp), Occur::MUST);
    bq.add(std::make_unique<TermQuery>("body", tn), Occur::SHOULD);
    bq.add(std::make_unique<TermQuery>("body", ts), Occur::MUST_NOT);

    auto scorer = bq.createScorer(ctx);
    if (!scorer) FAIL("createScorer 应非空");
    auto got = drainScorer(*scorer);

    for (DocId d : got) {
        if (d % 5 == 0) FAIL("结果包含了含 spam 的 doc " + std::to_string(d));
    }
    // 含 python 且不含 spam 的文档应全部命中
    int expected_count = 0;
    for (int i = 1; i <= 30; ++i)
        if (i % 5 != 0) ++expected_count;
    if ((int)got.size() != expected_count)
        FAIL("结果数量错误：期望=" + std::to_string(expected_count)
             + " 实际=" + std::to_string(got.size()));
    PASS();
    fs::remove_all(dir);
}

// ── 测试 2：嵌套 BooleanQuery（外层 MUST + 内层 SHOULD 子查询）──────────────

static void test_nested_boolean_query() {
    TEST("嵌套 BooleanQuery：外层 MUST + 内层 SHOULD");
    auto dir = tmpDir("nested");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 40; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body",  i % 2 == 0 ? "python" : "java");
            doc.set("title", i % 3 == 0 ? "tutorial" : "guide");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string tpython = stem("python"), tjava = "java",
                      ttutorial = stem("tutorial"), tguide = "guide";

    TermIdfs idfs = buildIdfs(seg, total, {
        {"body", tpython}, {"body", tjava},
        {"title", ttutorial}, {"title", tguide},
    });
    ScorerContext ctx{seg, idfs, total, nullptr, 100};

    // 内层：body:python SHOULD body:java（任意命中即可）
    auto inner = std::make_unique<BooleanQuery>();
    inner->add(std::make_unique<TermQuery>("body", tpython), Occur::SHOULD);
    inner->add(std::make_unique<TermQuery>("body", tjava),   Occur::SHOULD);

    // 外层：MUST 内层 AND MUST title:tutorial
    BooleanQuery outer;
    outer.add(std::move(inner), Occur::MUST);
    outer.add(std::make_unique<TermQuery>("title", ttutorial), Occur::MUST);

    auto scorer = outer.createScorer(ctx);
    if (!scorer) FAIL("嵌套 createScorer 应非空");
    auto got = std::set<DocId>(drainScorer(*scorer).begin(), drainScorer(*scorer).end());

    // 重新跑一遍（drainScorer 消耗了 scorer，需要重建）
    auto scorer2 = outer.createScorer(ctx);
    auto got_ids = drainScorer(*scorer2);

    // 期望：所有文档（body有内容 AND title:tutorial，即 3 的倍数）
    // body: 偶数=python, 奇数=java → 所有文档都命中内层 SHOULD
    // title: 3 的倍数=tutorial
    std::set<DocId> expected;
    for (int i = 1; i <= 40; ++i)
        if (i % 3 == 0) expected.insert(i);

    std::set<DocId> got_set(got_ids.begin(), got_ids.end());
    if (got_set != expected)
        FAIL("嵌套查询结果与预期不符");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 3：QueryParser → createScorer 与 IndexSearcher.search 结果一致 ──────

static void test_parser_matches_searcher() {
    TEST("QueryParser → createScorer 结果与 IndexSearcher.search 一致");
    auto dir = tmpDir("match");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 50; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 4 == 0) body += " deep";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }

    // IndexSearcher 路径
    IndexSearcher searcher(dir);
    auto searcher_res = searcher.search("python deep", 50, QueryMode::AND);
    auto searcher_ids = toIdSet(searcher_res);

    // 手动 QueryParser → BooleanQuery → Scorer 路径
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    Analyzer a;
    QueryParser parser(a, {"body"});
    auto bq = parser.parse("python deep", Occur::MUST);

    const std::string tp = stem("python"), td = "deep";
    TermIdfs idfs = buildIdfs(seg, total, {{"body", tp}, {"body", td}});
    ScorerContext ctx{seg, idfs, total, nullptr, 50};

    auto scorer = bq->createScorer(ctx);
    std::set<DocId> scorer_ids;
    if (scorer) {
        auto ids = drainScorer(*scorer);
        scorer_ids.insert(ids.begin(), ids.end());
    }

    if (searcher_ids != scorer_ids)
        FAIL("IndexSearcher 路径与手动 Scorer 路径结果不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 4：AND 模式裸词要求所有字段命中 ─────────────────────────────────────

static void test_and_bare_term_all_fields() {
    TEST("AND 模式裸词：要求在所有默认字段的任意一个命中（WANDScorer 内层 OR）");
    auto dir = tmpDir("and");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 30; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            // body: 偶数含 python; title: 3 的倍数含 python
            doc.set("body",  i % 2 == 0 ? "python" : "other");
            doc.set("title", i % 3 == 0 ? "python" : "none");
            w.addDocument(doc);
        }
        w.flush();
    }
    IndexSearcher searcher(dir);
    // AND 模式裸词 "python"：doc 必须在 body 或 title 中含 python
    auto results = searcher.search("python", 50, QueryMode::AND);
    auto got     = toIdSet(results);

    // 期望：body:python(偶数) OR title:python(3 的倍数)
    std::set<DocId> expected;
    for (int i = 1; i <= 30; ++i)
        if (i % 2 == 0 || i % 3 == 0) expected.insert(i);

    if (got != expected)
        FAIL("AND 裸词结果与预期（body OR title）不符");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 5：OR 模式结果为各 term 命中的并集子集 ──────────────────────────────

static void test_or_result_subset_of_union() {
    TEST("OR 模式：结果 ⊆ 各 term 命中的并集");
    auto dir = tmpDir("or");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 40; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body;
            if (i % 2 == 0) body += "alpha ";
            if (i % 3 == 0) body += "beta ";
            if (body.empty()) body = "other";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    TermIdfs idfs = buildIdfs(seg, total, {{"body","alpha"},{"body","beta"}});
    ScorerContext ctx{seg, idfs, total, nullptr, 50};

    std::vector<std::unique_ptr<Scorer>> scorers;
    scorers.push_back(std::make_unique<TermScorer>("body","alpha",ctx));
    scorers.push_back(std::make_unique<TermScorer>("body","beta", ctx));
    WANDScorer ws(std::move(scorers), 50, ctx);
    auto results = ws.collectTopK();

    // 所有结果必须在 alpha 或 beta 的倒排链中
    std::set<DocId> alpha_docs, beta_docs;
    { auto it = seg.postingIterator("body","alpha"); while(!it.isEnd()){alpha_docs.insert(it.docId());it.next();} }
    { auto it = seg.postingIterator("body","beta");  while(!it.isEnd()){beta_docs.insert(it.docId()); it.next();} }

    for (const auto& r : results) {
        if (!alpha_docs.count(r.doc_id) && !beta_docs.count(r.doc_id))
            FAIL("结果中出现了不在任何 term 命中中的 doc=" + std::to_string(r.doc_id));
    }
    PASS();
    fs::remove_all(dir);
}

// ── 测试 6：Merge 后搜索结果与合并前相同 ─────────────────────────────────────

static void test_merge_search_consistent() {
    TEST("Merge：合并后 search 结果与合并前一致");
    auto dir = tmpDir("msc");
    {
        IndexWriter w0(dir, 8);
        for (int i = 1; i <= 30; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 5 == 0) body += " tutorial";
            doc.set("body", body);
            w0.addDocument(doc);
        }
        w0.flush();
    }
    {
        IndexWriter w1(dir, 8);
        for (int i = 1; i <= 20; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 3 == 0) body += " tutorial";
            doc.set("body", body);
            w1.addDocument(doc);
        }
        w1.flush();
    }

    // 合并前：两个 Segment 搜索
    auto pre_res = [&]() {
        IndexSearcher s(dir);
        return toIdSet(s.search("python tutorial", 100, QueryMode::AND));
    }();

    // 合并
    SegmentMerger merger(dir);
    merger.mergeAll(2);

    // 合并后：单 Segment 搜索
    auto post_res = [&]() {
        IndexSearcher s(dir);
        return toIdSet(s.search("python tutorial", 100, QueryMode::AND));
    }();

    if (pre_res.size() != post_res.size())
        FAIL("合并前后命中数量不同：pre=" + std::to_string(pre_res.size())
             + " post=" + std::to_string(post_res.size()));
    // 结果数量一致即可（doc_id 会重编号，集合元素不同）
    PASS();
    fs::remove_all(dir);
}

// ── 测试 7：软删除 + Merge 后文档彻底消失 ────────────────────────────────────

static void test_soft_delete_after_merge() {
    TEST("软删除 + Merge：被删文档在合并后彻底消失");
    auto dir = tmpDir("del");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 20; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body", "python");
            doc.set("title", "doc" + std::to_string(i));
            w.addDocument(doc);
        }
        w.flush();
    }

    // 软删除 doc 5, 10, 15
    SegmentMerger pre_merger(dir);
    pre_merger.softDelete(5);
    pre_merger.softDelete(10);
    pre_merger.softDelete(15);
    pre_merger.mergeAll(1);

    IndexSearcher s(dir);
    auto results = s.search("python", 30, QueryMode::OR);

    // 合并后应只有 17 篇（20 - 3 被删）
    if ((int)results.size() != 17)
        FAIL("合并后文档数应为 17，实际=" + std::to_string(results.size()));
    PASS();
    fs::remove_all(dir);
}

// ── 测试 8：多 Segment 搜索汇总 ──────────────────────────────────────────────

static void test_multi_segment_search() {
    TEST("多 Segment 搜索：两个 Segment 结果正确汇总");
    auto dir = tmpDir("multi");
    {
        IndexWriter w0(dir, 8);
        for (int i = 1; i <= 15; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body", i % 2 == 0 ? "python" : "java");
            w0.addDocument(doc);
        }
        w0.flush();
    }
    {
        IndexWriter w1(dir, 8);
        for (int i = 1; i <= 10; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body", "python");
            w1.addDocument(doc);
        }
        w1.flush();
    }

    IndexSearcher s(dir);
    // Segment 0 有 7 篇 python，Segment 1 有 10 篇 python，共 17 篇
    auto results = s.search("python", 30, QueryMode::OR);
    if ((int)results.size() != 17)
        FAIL("多 Segment 搜索：python 应命中 17 篇，实际=" + std::to_string(results.size()));
    PASS();
    fs::remove_all(dir);
}

// ── 测试 9：QueryParser field:term 限定 vs 裸词展开行为不同 ──────────────────

static void test_field_term_vs_bare_term() {
    TEST("QueryParser field:term 限定 vs 裸词展开：结果集不同");
    auto dir = tmpDir("ft");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 20; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            // 偶数 body 含 python；奇数 title 含 python
            doc.set("body",  i % 2 == 0 ? "python" : "other");
            doc.set("title", i % 2 != 0 ? "python" : "none");
            w.addDocument(doc);
        }
        w.flush();
    }
    IndexSearcher s(dir);

    // body:python → 只有偶数 (10 篇)
    auto field_res = s.search("body:python", 30, QueryMode::OR);
    // 裸词 python（OR）→ body 或 title 含 python = 所有 20 篇
    auto bare_res  = s.search("python",      30, QueryMode::OR);

    if ((int)field_res.size() != 10)
        FAIL("field:term 限定应命中 10 篇，实际=" + std::to_string(field_res.size()));
    if ((int)bare_res.size() != 20)
        FAIL("裸词展开应命中 20 篇，实际=" + std::to_string(bare_res.size()));
    PASS();
    fs::remove_all(dir);
}

// ── 测试 10：空/停用词查询返回空结果 ─────────────────────────────────────────

static void test_empty_stopword_returns_empty() {
    TEST("空查询 / 停用词 → IndexSearcher 返回空结果");
    auto dir = tmpDir("empty");
    {
        IndexWriter w(dir, 8);
        Document doc; doc.doc_id = 1; doc.set("body", "python");
        w.addDocument(doc); w.flush();
    }
    IndexSearcher s(dir);

    // 空字符串
    if (!s.search("", 10, QueryMode::OR).empty())
        FAIL("空查询应返回空");

    // 停用词 "the"
    if (!s.search("the", 10, QueryMode::OR).empty())
        FAIL("停用词查询应返回空");

    // 不存在的词
    if (!s.search("xyznonexistent", 10, QueryMode::AND).empty())
        FAIL("不存在的词应返回空");

    PASS();
    fs::remove_all(dir);
}

// ── 入口 ─────────────────────────────────────────────────────────────────────

int main() {
    test_must_should_must_not();
    test_nested_boolean_query();
    test_parser_matches_searcher();
    test_and_bare_term_all_fields();
    test_or_result_subset_of_union();
    test_merge_search_consistent();
    test_soft_delete_after_merge();
    test_multi_segment_search();
    test_field_term_vs_bare_term();
    test_empty_stopword_returns_empty();

    std::cout << "\n═══════════════════════════════════════════════════\n";
    std::cout << "  All Scorer Tree E2E (Step 10) tests PASSED!\n";
    std::cout << "═══════════════════════════════════════════════════\n";
    return 0;
}
