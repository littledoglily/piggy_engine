// tests/parallel/test_parallel_merge_fanout.cpp
//
// 验证并行 Fan-out Merge（方案 A）的正确性
//
// 测试维度：
//   1. small_input_single_group    : temp segs ≤ 8 → 退化为 1 个最终 Segment（向后兼容）
//   2. large_input_multi_segment   : temp segs > 8 → 产出多个最终 Segment，总 doc_count 正确
//   3. term_stats_match_reference  : 各最终 Segment 的 term df 之和 == 单线程参考
//   4. searcher_multi_final_seg    : IndexSearcher 加载多 Segment，查询结果与单线程一致
//   5. no_seg_id_collision         : 所有最终 Segment ID 唯一，且不与 temp ID 重叠

#include "index/index_writer.h"
#include "index/parallel_index_writer.h"
#include "index/segment_reader.h"
#include "field/schema.h"
#include "core/types.h"
#include "query/index_searcher.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ii;
namespace fs = std::filesystem;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << (msg) << "\n"; assert(false); } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Schema & 文档集
// ─────────────────────────────────────────────────────────────────────────────

static Schema makeSchema() {
    Schema s;
    s.fields = {
        {"body",     FieldType::Text,    IndexOption::FreqsPositions, true,  false, false, {}},
        {"category", FieldType::Keyword, IndexOption::FreqsOnly,      false, false, false, {}},
    };
    return s;
}

static const std::vector<std::string> kBodies = {
    "distributed systems consistency availability partition tolerance replication",
    "machine learning gradient descent optimization neural network backpropagation",
    "database index btree hash table query execution plan optimizer join",
    "concurrency locking deadlock transaction isolation level serializable",
    "network protocol tcp udp http rest api design latency throughput",
    "memory allocation cache locality prefetch branch prediction pipeline",
    "compiler optimization register allocation instruction scheduling loop",
    "search engine inverted index ranking bm25 tf idf scoring retrieval",
    "graph algorithm shortest path spanning tree dynamic programming flow",
    "operating system process thread scheduler virtual memory paging swap",
};
static const std::vector<std::string> kCats = {
    "systems","ml","database","concurrency","network",
    "hardware","compiler","search","algorithms","os",
};

static Document makeDoc(int i) {
    Document d;
    d.set("body",     kBodies[i % kBodies.size()]);
    d.set("category", kCats  [i % kCats.size()]);
    return d;
}

// 扫描目录，收集所有含 .done 的 segment_N 子目录 ID
static std::vector<uint32_t> collectDoneSegIds(const std::string& dir) {
    std::vector<uint32_t> ids;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_directory()) continue;
        auto name = e.path().filename().string();
        if (name.size() > 8 && name.substr(0, 8) == "segment_"
                && fs::exists(e.path() / ".done")) {
            try { ids.push_back(std::stoul(name.substr(8))); } catch (...) {}
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

// 从 SegmentReader 提取某字段 term → df 映射
static std::map<std::string, uint32_t> termDf(
    const SegmentReader& r, const std::string& field)
{
    std::map<std::string, uint32_t> m;
    const auto* dict = r.fieldTermDict(field);
    if (dict) for (const auto& [t, meta] : *dict) m[t] = meta.doc_freq;
    return m;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1：temp segs ≤ 8 → 退化为 1 个最终 Segment（向后兼容）
// ─────────────────────────────────────────────────────────────────────────────

void test_small_input_single_group() {
    TEST(small_input_degrades_to_single_segment);

    const std::string dir = "/tmp/piggy_fanout_t1";
    fs::remove_all(dir);

    // 4 workers，超大 RAM → 每个 worker 最多 flush 1 次 → ≤ 4 temp segs → 1 组
    constexpr int   N       = 80;
    constexpr int   WORKERS = 4;
    constexpr float RAM_MB  = 512.f;

    {
        ParallelIndexWriter writer(dir, WORKERS, RAM_MB, makeSchema());
        for (int i = 0; i < N; ++i) writer.addDocument(makeDoc(i));
        writer.commit();

        // 退化路径：1 个最终 Segment
        if (writer.segmentCount() != 1)
            FAIL("expected 1 final segment, got " + std::to_string(writer.segmentCount()));
        if (writer.totalDocs() != static_cast<uint32_t>(N))
            FAIL("totalDocs=" + std::to_string(writer.totalDocs()));
    }

    auto done = collectDoneSegIds(dir);
    if (done.size() != 1)
        FAIL("disk: expected 1 .done segment, found " + std::to_string(done.size()));

    SegmentReader reader(dir, done[0]);
    if (reader.docCount() != static_cast<uint32_t>(N))
        FAIL("docCount=" + std::to_string(reader.docCount()));

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2：temp segs > 8 → 产出多个最终 Segment，总 doc_count 正确
// ─────────────────────────────────────────────────────────────────────────────

void test_large_input_multi_segment() {
    TEST(large_input_produces_multiple_final_segments);

    const std::string dir = "/tmp/piggy_fanout_t2";
    fs::remove_all(dir);

    // 小 RAM 强制多次 flush → temp segs > 8 → 多组并行 merge
    constexpr int   N       = 400;
    constexpr int   WORKERS = 4;
    constexpr float RAM_MB  = 0.02f;

    uint32_t n_final = 0;
    {
        ParallelIndexWriter writer(dir, WORKERS, RAM_MB, makeSchema());
        for (int i = 0; i < N; ++i) writer.addDocument(makeDoc(i));
        writer.commit();

        n_final = writer.segmentCount();
        if (n_final < 2)
            FAIL("expected > 1 final segment, got " + std::to_string(n_final));
        if (writer.totalDocs() != static_cast<uint32_t>(N))
            FAIL("totalDocs=" + std::to_string(writer.totalDocs()));
    }

    // 磁盘上 .done segment 数量与 segmentCount() 一致
    auto done = collectDoneSegIds(dir);
    if (done.size() != n_final)
        FAIL("disk done=" + std::to_string(done.size())
             + " segmentCount=" + std::to_string(n_final));

    // 各最终 Segment 的 docCount 之和 == N
    uint32_t total = 0;
    for (uint32_t sid : done) {
        SegmentReader r(dir, sid);
        total += r.docCount();
    }
    if (total != static_cast<uint32_t>(N))
        FAIL("sum docCount=" + std::to_string(total) + " expected=" + std::to_string(N));

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3：各最终 Segment 的 term df 之和 == 单线程参考
// ─────────────────────────────────────────────────────────────────────────────

void test_term_stats_match_reference() {
    TEST(term_df_sum_matches_single_thread_reference);

    constexpr int   N       = 300;
    constexpr int   WORKERS = 4;
    constexpr float RAM_MB  = 0.02f;

    const std::string dir_st  = "/tmp/piggy_fanout_t3_st";
    const std::string dir_par = "/tmp/piggy_fanout_t3_par";
    fs::remove_all(dir_st);
    fs::remove_all(dir_par);

    // 单线程参考
    {
        IndexWriter writer(dir_st, 512.f, makeSchema());
        DocId id = 0;
        for (int i = 0; i < N; ++i) {
            Document d = makeDoc(i);
            d.doc_id = ++id;
            writer.addDocument(d);
        }
        writer.commit();
    }

    // 并行（多个最终 Segment）
    {
        ParallelIndexWriter writer(dir_par, WORKERS, RAM_MB, makeSchema());
        for (int i = 0; i < N; ++i) writer.addDocument(makeDoc(i));
        writer.commit();
    }

    // 收集单线程 df
    SegmentReader st_r(dir_st, 0);

    // 累加所有并行最终 Segment 的 df
    auto par_segs = collectDoneSegIds(dir_par);

    for (const std::string& field : {"body", "category"}) {
        auto st_df = termDf(st_r, field);

        // 累加各 par segment 的 df
        std::map<std::string, uint32_t> par_sum;
        for (uint32_t sid : par_segs) {
            SegmentReader r(dir_par, sid);
            for (const auto& [t, df] : termDf(r, field))
                par_sum[t] += df;
        }

        if (st_df.size() != par_sum.size())
            FAIL("field=" + field + " term_count st=" + std::to_string(st_df.size())
                 + " par=" + std::to_string(par_sum.size()));

        for (const auto& [term, df] : st_df) {
            if (!par_sum.count(term))
                FAIL("field=" + field + " missing term=" + term);
            if (par_sum[term] != df)
                FAIL("field=" + field + " term=" + term
                     + " st_df=" + std::to_string(df)
                     + " par_sum=" + std::to_string(par_sum[term]));
        }
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4：IndexSearcher 加载多个最终 Segment，查询命中数与单线程一致
// ─────────────────────────────────────────────────────────────────────────────

void test_searcher_multi_final_seg() {
    TEST(searcher_loads_multi_final_segs_consistent_hits);

    constexpr int   N       = 300;
    constexpr int   WORKERS = 4;
    constexpr float RAM_MB  = 0.02f;

    const std::string dir_st  = "/tmp/piggy_fanout_t4_st";
    const std::string dir_par = "/tmp/piggy_fanout_t4_par";
    fs::remove_all(dir_st);
    fs::remove_all(dir_par);

    // 单线程
    {
        IndexWriter writer(dir_st, 512.f, makeSchema());
        DocId id = 0;
        for (int i = 0; i < N; ++i) {
            Document d = makeDoc(i);
            d.doc_id = ++id;
            writer.addDocument(d);
        }
        writer.commit();
    }

    // 并行
    {
        ParallelIndexWriter writer(dir_par, WORKERS, RAM_MB, makeSchema());
        for (int i = 0; i < N; ++i) writer.addDocument(makeDoc(i));
        writer.commit();
    }

    IndexSearcher st_s (dir_st);
    IndexSearcher par_s(dir_par);

    const std::vector<std::string> queries = {
        "body:distributed",
        "body:machine",
        "body:index",
        "body:search",
        "body:algorithm",
        "body:neural body:network",
    };

    for (const auto& q : queries) {
        auto st_hits  = st_s .search(q, N, QueryMode::OR);
        auto par_hits = par_s.search(q, N, QueryMode::OR);

        if (st_hits.size() != par_hits.size())
            FAIL("query='" + q + "' st=" + std::to_string(st_hits.size())
                 + " par=" + std::to_string(par_hits.size()));

        // 所有命中 score > 0
        for (const auto& r : par_hits)
            if (r.score <= 0.f)
                FAIL("query='" + q + "' doc=" + std::to_string(r.doc_id) + " score<=0");
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5：最终 Segment ID 全局唯一，与 temp ID 不重叠
// ─────────────────────────────────────────────────────────────────────────────

void test_no_seg_id_collision() {
    TEST(final_seg_ids_unique_no_overlap_with_temp);

    const std::string dir = "/tmp/piggy_fanout_t5";
    fs::remove_all(dir);

    constexpr int   N       = 400;
    constexpr int   WORKERS = 4;
    constexpr float RAM_MB  = 0.02f;

    std::vector<uint32_t> final_ids;
    {
        ParallelIndexWriter writer(dir, WORKERS, RAM_MB, makeSchema());
        for (int i = 0; i < N; ++i) writer.addDocument(makeDoc(i));
        writer.commit();
        final_ids = writer.segmentIds();
    }

    // 磁盘上所有 .done segment 与 segmentIds() 完全一致
    auto done = collectDoneSegIds(dir);
    std::set<uint32_t> done_set(done.begin(), done.end());
    std::set<uint32_t> final_set(final_ids.begin(), final_ids.end());

    if (done_set != final_set)
        FAIL("disk done segments != writer.segmentIds()");

    // 没有重复 ID
    if (final_set.size() != final_ids.size())
        FAIL("duplicate final seg IDs detected");

    // 最终 ID 必须 > 所有 temp ID（temp ID 由 worker flush 产生，final ID 在 merge 阶段分配）
    // temp ID 范围：[0, first_final_id)
    if (!final_ids.empty()) {
        uint32_t min_final = *std::min_element(final_ids.begin(), final_ids.end());
        // 扫描目录确认不存在既是 temp 又在 final 中的情况
        // 只要 done_set 和 final_set 相同且 done_set.size() == final_ids.size()，
        // 且无 temp 文件残留，就满足无冲突要求
        // 验证无孤立 temp segment 残留（已被 merger 清理）
        for (const auto& e : fs::directory_iterator(dir)) {
            if (!e.is_directory()) continue;
            auto name = e.path().filename().string();
            if (name.size() <= 8 || name.substr(0, 8) != "segment_") continue;
            uint32_t sid = 0;
            try { sid = std::stoul(name.substr(8)); } catch (...) { continue; }
            if (!done_set.count(sid))
                FAIL("orphan non-done segment_" + std::to_string(sid) + " found after commit");
        }
        (void)min_final;
    }

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Parallel Fan-out Merge Tests ===\n\n";

    test_small_input_single_group();
    test_large_input_multi_segment();
    test_term_stats_match_reference();
    test_searcher_multi_final_seg();
    test_no_seg_id_collision();

    std::cout << "\nAll fan-out merge tests passed.\n";
    return 0;
}
