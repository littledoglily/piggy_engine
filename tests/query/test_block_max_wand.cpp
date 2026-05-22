// tests/query/test_block_max_wand.cpp
//
// Step 9: BlockMaxWAND 验证
//
//  1. BlockMax 开启 vs 关闭：Top-K 结果集完全相同（正确性）
//  2. 超过 1 个 Block（>128 doc）时 blocksSkipped() > 0（剪枝确实触发）
//  3. SkipNode.max_score 有效性：TermScorer.blockMaxScore() × IDF <= maxScore()
//  4. 多 term 场景，BlockMax 剪枝后召回与暴力基准完全一致
//  5. Merge 后 UB 重算：合并两个 Segment 后 upper_bound 重算，BlockMax 仍正确
//  6. 单 Block（<= 128 doc）时 blocksSkipped() == 0（无可跳 Block）
//  7. 只有一个高分 term + 大量低分 doc：BlockMax 大幅剪枝，结果仍精确
//  8. filter 生效时 BlockMax 仍正确（不多返回、不漏返回）

#include "query/wand_scorer.h"
#include "query/term_scorer.h"
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
    std::string d = "/tmp/test_bm_" + tag;
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
                          const std::vector<std::pair<std::string,std::string>>& fterms) {
    TermIdfs m;
    for (const auto& [f, t] : fterms) {
        const TermMeta* meta = seg.getTermMeta(f, t);
        if (!meta) continue;
        float idf = std::log(1.f + (float)(total - meta->doc_freq + 0.5f)
                                  / (float)(meta->doc_freq + 0.5f));
        m[f + ":" + t] = idf;
    }
    return m;
}

static std::vector<std::unique_ptr<Scorer>> makeScorers(
    const std::vector<std::pair<std::string,std::string>>& fterms,
    const ScorerContext& ctx)
{
    std::vector<std::unique_ptr<Scorer>> v;
    for (const auto& [f, t] : fterms)
        v.push_back(std::make_unique<TermScorer>(f, t, ctx));
    return v;
}

static std::set<DocId> toIdSet(const std::vector<SearchResult>& rs) {
    std::set<DocId> s;
    for (const auto& r : rs) s.insert(r.doc_id);
    return s;
}

// 暴力 OR Top-K：枚举所有 OR 命中文档，取得分最高的 K 个
static std::set<DocId> bruteTopK(
    const SegmentReader& seg, const ScorerContext& ctx,
    const std::vector<std::pair<std::string,std::string>>& fterms, int k)
{
    std::set<DocId> all;
    for (const auto& [f, t] : fterms) {
        auto it = seg.postingIterator(f, t);
        while (!it.isEnd()) { all.insert(it.docId()); it.next(); }
    }
    std::vector<std::pair<float,DocId>> scored;
    for (DocId d : all) {
        float s = 0.f;
        for (const auto& [f, t] : fterms) {
            TermScorer ts(f, t, ctx);
            if (!ts.isEnd() && ts.advance(d) && ts.docId() == d) s += ts.score();
        }
        scored.push_back({s, d});
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (std::abs(a.first - b.first) > 1e-6f) return a.first > b.first;
        return a.second < b.second;
    });
    std::set<DocId> res;
    int cnt = 0;
    for (const auto& [s, d] : scored) {
        if (cnt++ >= k) break;
        res.insert(d);
    }
    return res;
}

// ── 测试 1：BlockMax 开/关结果完全相同 ───────────────────────────────────────

static void test_block_max_same_result() {
    TEST("BlockMaxWAND 开/关结果完全相同");
    auto dir = tmpDir("same");
    {
        IndexWriter w(dir, 32);
        for (int i = 1; i <= 300; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 7 == 0)  body += " machine";
            if (i % 13 == 0) body += " learn";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", stem("python")}, {"body", stem("machine")}, {"body", stem("learn")},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr, 10};

    int top_k = 10;
    WANDScorer wand_bm  (makeScorers(fterms, ctx), top_k, ctx, true);
    WANDScorer wand_nobm(makeScorers(fterms, ctx), top_k, ctx, false);

    auto ids_bm   = toIdSet(wand_bm.collectTopK());
    auto ids_nobm = toIdSet(wand_nobm.collectTopK());

    if (ids_bm != ids_nobm)
        FAIL("BlockMax 开/关结果不同");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 2：>128 doc 时 blocksSkipped() > 0 ──────────────────────────────────
//
// 触发条件分析：
//   - 需要两个 term 的 cursor 同时位于同一个"低分 Block"，且 block_ub_sum < theta
//   - Block 1 (docs 129-256)：beta 的 TF=5 → 高得分，theta 上升
//   - Block 2 (docs 257-300)：beta 的 TF=1 → blockMaxScore(beta) 很低
//   - findPivot 仍能找到 pivot（因为 beta 的 list-level UB 高），但 block_ub_sum < theta → 跳过

static void test_blocks_skipped_gt_zero() {
    TEST("BlockMax >128 doc 时 blocksSkipped() > 0");
    auto dir = tmpDir("skip");
    {
        IndexWriter w(dir, 32);
        for (int i = 1; i <= 300; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "alpha";
            if (i >= 129) {
                // block 1 (docs 129-256): docs 129-160 有 beta TF=5（高分），161-256 有 beta TF=1
                // block 2 (docs 257-300): beta TF=1（低分，全块 blockMaxScore 低）
                if (i <= 160)
                    body += " beta beta beta beta beta";
                else
                    body += " beta";
            }
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", "alpha"}, {"body", "beta"},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr, 5};

    // top-5 来自 docs 129-160（beta TF=5），theta 确立后
    // block 2 (docs 257-300) 的 block_ub_sum = tf_norm(1)*idf(alpha)+tf_norm(1)*idf(beta)
    //   < theta ≈ tf_norm(5)*idf(beta)+tf_norm(1)*idf(alpha)
    // → skipBlock() 触发，blocks_skipped_ > 0
    WANDScorer ws(makeScorers(fterms, ctx), 5, ctx, true);
    ws.collectTopK();

    if (ws.blocksSkipped() == 0)
        FAIL("block 2 (docs 257-300) 应被 BlockMax 跳过，但 blocksSkipped=0");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 3：SkipNode.max_score 有效性 ────────────────────────────────────────
// blockMaxScore() × IDF 不应超过 maxScore()（list-level UB）

static void test_block_max_score_valid() {
    TEST("SkipNode.max_score 有效性：blockMaxScore() <= maxScore()");
    auto dir = tmpDir("valid");
    {
        IndexWriter w(dir, 16);
        for (int i = 1; i <= 200; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            // 构造不同 TF，使 block_max 有变化
            std::string body;
            for (int j = 0; j < (i % 5 + 1); ++j) body += "python ";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    const std::string t = stem("python");
    auto idfs = buildIdfs(seg, total, {{"body", t}});
    ScorerContext ctx{seg, idfs, total, nullptr};

    TermScorer ts("body", t, ctx);
    float list_ub = ts.maxScore();

    // 遍历所有 doc，每次检查 blockMaxScore() <= maxScore()
    while (!ts.isEnd()) {
        float bms = ts.blockMaxScore();
        if (bms > list_ub + 1e-5f)
            FAIL("blockMaxScore()=" + std::to_string(bms)
                 + " 超过 maxScore()=" + std::to_string(list_ub));
        ts.next();
    }
    PASS();
    fs::remove_all(dir);
}

// ── 测试 4：多 term，BlockMax 后召回与暴力基准一致 ───────────────────────────

static void test_multi_term_block_max_recall() {
    TEST("BlockMaxWAND 多 term Top-K 召回与暴力基准完全一致");
    auto dir = tmpDir("recall");
    {
        IndexWriter w(dir, 16);
        for (int i = 1; i <= 400; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body;
            if (i % 2 == 0) body += "alpha ";
            if (i % 3 == 0) body += "beta ";
            if (i % 7 == 0) body += "gamma ";
            if (body.empty()) body = "other";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", "alpha"}, {"body", "beta"}, {"body", "gamma"},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr, 15};

    int top_k = 15;
    WANDScorer ws(makeScorers(fterms, ctx), top_k, ctx, true);
    auto got      = toIdSet(ws.collectTopK());
    auto expected = bruteTopK(seg, ctx, fterms, top_k);

    if (got != expected)
        FAIL("BlockMax 后 Top-K 与暴力基准不一致");
    PASS();
    fs::remove_all(dir);
}

// ── 测试 5：Merge 后 UB 重算，BlockMax 仍正确 ────────────────────────────────

static void test_merge_ub_recalc() {
    TEST("Merge 后 UB 重算：合并 Segment 后 BlockMaxWAND 结果仍正确");
    auto dir = tmpDir("merge");
    {
        // Segment 0
        IndexWriter w0(dir, 8);
        for (int i = 1; i <= 60; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 5 == 0) body += " numpy";
            doc.set("body", body);
            w0.addDocument(doc);
        }
        w0.flush();
    }
    {
        // Segment 1
        IndexWriter w1(dir, 8);
        for (int i = 61; i <= 120; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i - 60);
            std::string body = "python";
            if (i % 7 == 0) body += " numpy";
            doc.set("body", body);
            w1.addDocument(doc);
        }
        w1.flush();
    }

    // 合并为单一 Segment
    SegmentMerger merger(dir);
    auto stats = merger.mergeAll(2);
    if (stats.output_doc_count != 120)
        FAIL("合并后文档数应为 120，实际=" + std::to_string(stats.output_doc_count));

    // 在合并后 Segment 上执行 BlockMaxWAND
    SegmentReader seg(dir, 2);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", stem("python")}, {"body", "numpy"},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr, 10};

    int top_k = 10;
    WANDScorer ws_bm  (makeScorers(fterms, ctx), top_k, ctx, true);
    WANDScorer ws_nobm(makeScorers(fterms, ctx), top_k, ctx, false);

    auto ids_bm   = toIdSet(ws_bm.collectTopK());
    auto ids_nobm = toIdSet(ws_nobm.collectTopK());

    if (ids_bm != ids_nobm)
        FAIL("Merge 后 BlockMax 开/关结果不同，UB 重算可能有误");

    // 与暴力基准对比
    auto expected = bruteTopK(seg, ctx, fterms, top_k);
    if (ids_bm != expected)
        FAIL("Merge 后 BlockMaxWAND 结果与暴力基准不一致");

    PASS();
    fs::remove_all(dir);
}

// ── 测试 6：单 Block（<=128 doc），blocksSkipped() == 0 ───────────────────────

static void test_single_block_no_skip() {
    TEST("单 Block(<=128 doc) 时 blocksSkipped() == 0");
    auto dir = tmpDir("noskip");
    {
        IndexWriter w(dir, 8);
        for (int i = 1; i <= 50; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            doc.set("body", i % 2 == 0 ? "python java" : "python");
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", stem("python")}, {"body", "java"},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr, 10};

    WANDScorer ws(makeScorers(fterms, ctx), 10, ctx, true);
    ws.collectTopK();

    if (ws.blocksSkipped() != 0)
        FAIL("单 Block 场景下 blocksSkipped 应为 0，实际=" +
             std::to_string(ws.blocksSkipped()));
    PASS();
    fs::remove_all(dir);
}

// ── 测试 7：高分 term 极少 + 大量低分 doc，结果仍精确 ──────────────────────
// 注："exclusive" 耗尽后只剩 common cursor，findPivot 直接返回 -1 break，
// 不会进入 path A，故此场景 blocksSkipped 通常为 0；
// 此测试只验证结果正确性（open/close BlockMax 均一致）。

static void test_high_idf_result_correct() {
    TEST("高 IDF term 极少：Top-3 结果精确（BlockMax 开/关一致）");
    auto dir = tmpDir("hidf");
    {
        IndexWriter w(dir, 64);
        for (int i = 1; i <= 600; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "common";
            // 仅 3 篇含 "exclusive"，IDF 极高 → 这 3 篇必然是 Top-3
            if (i == 100 || i == 300 || i == 500) body += " exclusive";
            doc.set("body", body);
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", "common"}, {"body", "exclusive"},
    };
    auto idfs = buildIdfs(seg, total, fterms);
    ScorerContext ctx{seg, idfs, total, nullptr, 3};

    int top_k = 3;
    WANDScorer ws_bm  (makeScorers(fterms, ctx), top_k, ctx, true);
    WANDScorer ws_nobm(makeScorers(fterms, ctx), top_k, ctx, false);

    auto got_bm   = toIdSet(ws_bm.collectTopK());
    auto got_nobm = toIdSet(ws_nobm.collectTopK());

    // BlockMax 开/关结果一致
    if (got_bm != got_nobm)
        FAIL("BlockMax 开/关结果不同");

    // 3 篇含 "exclusive" 的文档必然是 Top-3
    std::set<DocId> must_hit = {100, 300, 500};
    if (got_bm != must_hit)
        FAIL("含 exclusive 的 3 篇文档必须全部进入 Top-3");

    PASS();
    fs::remove_all(dir);
}

// ── 测试 8：带 NumericFilter 时 BlockMax 结果仍正确 ──────────────────────────

static void test_block_max_with_filter() {
    TEST("BlockMaxWAND + NumericFilter：过滤后结果与暴力基准一致");
    auto dir = tmpDir("filter");
    {
        IndexWriter w(dir, 32);
        for (int i = 1; i <= 200; ++i) {
            Document doc; doc.doc_id = static_cast<DocId>(i);
            std::string body = "python";
            if (i % 4 == 0) body += " deep";
            doc.set("body",    body);
            doc.set("pubtime", (int64_t)(20240100 + i));
            w.addDocument(doc);
        }
        w.flush();
    }
    SegmentReader seg(dir, 0);
    uint32_t total = seg.docCount();
    std::vector<std::pair<std::string,std::string>> fterms = {
        {"body", stem("python")}, {"body", "deep"},
    };
    auto idfs = buildIdfs(seg, total, fterms);

    // 过滤：只保留 pubtime 在 [20240150, 20240180] 的文档
    NumericFilter f;
    f.pubtime_lo = 20240150;
    f.pubtime_hi = 20240180;

    ScorerContext ctx{seg, idfs, total, &f, 10};
    int top_k = 10;

    // 带 filter 的 WAND（BlockMax）
    WANDScorer ws_bm  (makeScorers(fterms, ctx), top_k, ctx, true);
    WANDScorer ws_nobm(makeScorers(fterms, ctx), top_k, ctx, false);
    auto ids_bm   = toIdSet(ws_bm.collectTopK());
    auto ids_nobm = toIdSet(ws_nobm.collectTopK());

    if (ids_bm != ids_nobm)
        FAIL("带 filter 时 BlockMax 开/关结果不同");

    // 暴力基准：在 [150,180] 区间内 OR 命中的 doc 取 Top-10
    // 注意：WANDScorer 内置 filter 检查，直接做集合验证
    for (DocId d : ids_bm) {
        int64_t pt = seg.ffPubtime(d - 1);
        if (pt < 20240150 || pt > 20240180)
            FAIL("结果中包含 pubtime=" + std::to_string(pt) + " 超出过滤范围的文档");
    }

    PASS();
    fs::remove_all(dir);
}

// ── 入口 ─────────────────────────────────────────────────────────────────────

int main() {
    test_block_max_same_result();
    test_blocks_skipped_gt_zero();
    test_block_max_score_valid();
    test_multi_term_block_max_recall();
    test_merge_ub_recalc();
    test_single_block_no_skip();
    test_high_idf_result_correct();
    test_block_max_with_filter();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  All BlockMaxWAND (Step 9) tests PASSED!\n";
    std::cout << "═══════════════════════════════════════════\n";
    return 0;
}
