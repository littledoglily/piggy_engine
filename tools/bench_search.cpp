// tools/bench_search.cpp
//
// 检索性能基准测试（lazy iterator 实现前的摸底数据）
//
// 用法:
//   bench_search --index <dir> [options]
//
// Options:
//   --index  <path>   索引目录（必填）
//   --warmup <N>      预热轮数（默认 3）
//   --repeat <N>      每项测试重复次数（默认 200）
//   --top    <N>      搜索 top-k（默认 10）
//
// 测试项:
//   [A] readPostingList 微基准：按 df 分 4 个桶，各取代表性 term 测延迟和内存
//   [B] bm25Score 调用次数：统计单次 search() 内 readPostingList 被调用几次
//   [C] 端到端 search() 延迟：AND / OR，1~3 词查询
//   [D] 吞吐量：QPS（循环执行所有查询，统计总量）

#include "query/index_searcher.h"
#include "segment/segment_reader.h"
#include "types.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;
using us_t   = long long;   // microseconds

// ─────────────────────────────────────────────────────────────────────────────
// 静默 stdout（压制 search() 内部打印）
// ─────────────────────────────────────────────────────────────────────────────

struct SilentGuard {
    std::streambuf* saved_;
    explicit SilentGuard() : saved_(std::cout.rdbuf()) {
        std::cout.rdbuf(null_.rdbuf());
    }
    ~SilentGuard() { std::cout.rdbuf(saved_); }
private:
    std::ostringstream null_;
};

// ─────────────────────────────────────────────────────────────────────────────
// 计时工具
// ─────────────────────────────────────────────────────────────────────────────

static us_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now().time_since_epoch()).count();
}

struct Stats {
    std::vector<us_t> samples;

    void record(us_t v) { samples.push_back(v); }

    void compute(us_t& mn, us_t& p50, us_t& p95, us_t& p99, us_t& mx, double& avg) const {
        auto v = samples;
        std::sort(v.begin(), v.end());
        mn  = v.front();
        mx  = v.back();
        p50 = v[v.size() * 50 / 100];
        p95 = v[v.size() * 95 / 100];
        p99 = v[v.size() * 99 / 100];
        avg = static_cast<double>(std::accumulate(v.begin(), v.end(), 0LL)) / v.size();
    }

    void print(const std::string& label, const std::string& unit = "us") const {
        us_t mn, p50, p95, p99, mx; double avg;
        compute(mn, p50, p95, p99, mx, avg);
        printf("  %-36s  min=%-6lld  p50=%-6lld  p95=%-6lld  p99=%-6lld  max=%-6lld  avg=%.1f  [%s]\n",
               label.c_str(), mn, p50, p95, p99, mx, avg, unit.c_str());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 参数
// ─────────────────────────────────────────────────────────────────────────────

struct Args {
    std::string index_dir;
    int warmup  = 3;
    int repeat  = 200;
    int top_k   = 10;
};

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --index <dir> [--warmup N] [--repeat N] [--top N]\n";
    std::exit(1);
}

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string f = argv[i];
        if      (f == "--index"  && i+1 < argc) a.index_dir = argv[++i];
        else if (f == "--warmup" && i+1 < argc) a.warmup    = std::stoi(argv[++i]);
        else if (f == "--repeat" && i+1 < argc) a.repeat    = std::stoi(argv[++i]);
        else if (f == "--top"    && i+1 < argc) a.top_k     = std::stoi(argv[++i]);
        else usage(argv[0]);
    }
    if (a.index_dir.empty()) usage(argv[0]);
    return a;
}

// ─────────────────────────────────────────────────────────────────────────────
// 工具函数
// ─────────────────────────────────────────────────────────────────────────────

static std::string dfBucket(uint32_t df) {
    if (df < 10)   return "low    (df<10)";
    if (df < 100)  return "mid    (df<100)";
    if (df < 1000) return "high   (df<1000)";
    return                 "vhigh  (df>=1000)";
}

// 从 term_dict 中按 df 分桶，各取 top-N 代表性 term
static std::vector<std::pair<std::string, uint32_t>>
pickTerms(const std::map<std::string, ii::TermMeta>& dict,
          uint32_t df_lo, uint32_t df_hi, int max_pick)
{
    std::vector<std::pair<std::string, uint32_t>> bucket;
    for (const auto& [term, meta] : dict) {
        if (meta.doc_freq >= df_lo && meta.doc_freq < df_hi)
            bucket.push_back({term, meta.doc_freq});
    }
    std::sort(bucket.begin(), bucket.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if ((int)bucket.size() > max_pick) bucket.resize(max_pick);
    return bucket;
}

static void printSeparator(char c = '-', int n = 80) {
    std::cout << std::string(n, c) << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// [A] readPostingList 微基准
//
// 测量目标：当前全量解压路径的延迟和内存开销
// 对比点：lazy iterator 实现后，同样访问完整 posting 的耗时对比
// ─────────────────────────────────────────────────────────────────────────────

static void benchReadPostingList(const ii::SegmentReader& seg, const Args& args) {
    printSeparator('=');
    std::cout << "[A] readPostingList 微基准\n";
    std::cout << "    当前行为：所有 Block 一次性解压到 vector<DocId>，内存 O(df)\n";
    printSeparator();

    const auto& dict = seg.termDict();

    // df 分桶：(lo, hi, 描述, 最多取 N 个 term)
    struct Bucket { uint32_t lo, hi; const char* label; int pick; };
    const std::vector<Bucket> buckets = {
        {1,    10,    "low    df=[1,10)",    3},
        {10,   100,   "mid    df=[10,100)",  3},
        {100,  1000,  "high   df=[100,1000)",3},
        {1000, UINT32_MAX, "vhigh  df>=1000", 3},
    };

    printf("  %-16s  %-20s  %6s  %8s  %9s  %9s\n",
           "df-bucket", "term", "df", "vec_KB", "lat_us(p50)", "lat_us(p99)");
    printSeparator();

    for (const auto& bk : buckets) {
        auto terms = pickTerms(dict, bk.lo, bk.hi, bk.pick);
        if (terms.empty()) {
            printf("  %-16s  (no terms in this bucket)\n", bk.label);
            continue;
        }

        for (const auto& [term, df] : terms) {
            // 预热
            for (int w = 0; w < args.warmup; ++w)
                (void)seg.readPostingList(term);

            Stats st;
            for (int r = 0; r < args.repeat; ++r) {
                auto t0 = now_us();
                auto docs = seg.readPostingList(term);
                auto t1 = now_us();
                st.record(t1 - t0);
                (void)docs;
            }

            // 内存估算：vector<DocId>，每个 DocId = 4B
            double vec_kb = df * sizeof(ii::DocId) / 1024.0;

            us_t mn, p50, p95, p99, mx; double avg;
            st.compute(mn, p50, p95, p99, mx, avg);

            std::string display = term.size() > 18 ? term.substr(0, 15) + "..." : term;
            printf("  %-16s  %-20s  %6u  %8.2f  %9lld  %9lld\n",
                   bk.label, display.c_str(), df, vec_kb, p50, p99);
        }
    }

    // 汇总：全量加载所有 term 的总内存估算
    uint64_t total_docs_in_lists = 0;
    for (const auto& [t, m] : dict) total_docs_in_lists += m.doc_freq;
    printf("\n  索引总 posting 条目: %llu  (若全部解压到 vector: %.1f MB)\n",
           (unsigned long long)total_docs_in_lists,
           total_docs_in_lists * sizeof(ii::DocId) / 1024.0 / 1024.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// [B] bm25Score 冗余 readPostingList 计数
//
// 当前 bm25Score 内部对每个 term 再次调用 readPostingList，
// 这意味着每次精确打分都会做一次全量解压。
// 该测试通过时间分布估算这部分开销在总 search() 中的占比。
// ─────────────────────────────────────────────────────────────────────────────

static void benchBm25Overhead(const ii::SegmentReader& seg,
                               const std::vector<std::string>& query_terms,
                               const std::unordered_map<std::string, float>& term_idfs,
                               const Args& args)
{
    // 找出查询 term 命中的 doc（AND 交集）
    std::vector<ii::DocId> candidates;
    {
        bool first = true;
        for (const auto& t : query_terms) {
            auto docs = seg.readPostingList(t);
            if (first) { candidates = docs; first = false; }
            else {
                std::vector<ii::DocId> inter;
                std::set_intersection(candidates.begin(), candidates.end(),
                                      docs.begin(), docs.end(),
                                      std::back_inserter(inter));
                candidates = inter;
            }
        }
    }

    if (candidates.empty()) return;

    printf("\n  bm25Score 冗余开销分析  (query=[");
    for (size_t i = 0; i < query_terms.size(); ++i) {
        if (i) printf(", ");
        printf("%s", query_terms[i].c_str());
    }
    printf("]  命中docs=%zu)\n", candidates.size());

    // 测量单次 bm25Score 调用耗时
    Stats st_bm25;
    for (int w = 0; w < args.warmup; ++w)
        (void)seg.bm25Score(candidates[0], query_terms, term_idfs);

    for (int r = 0; r < args.repeat; ++r) {
        for (ii::DocId did : candidates) {
            auto t0 = now_us();
            (void)seg.bm25Score(did, query_terms, term_idfs);
            auto t1 = now_us();
            st_bm25.record(t1 - t0);
        }
    }

    us_t mn, p50, p95, p99, mx; double avg;
    st_bm25.compute(mn, p50, p95, p99, mx, avg);
    printf("  bm25Score/call: min=%-4lld  p50=%-4lld  p99=%-4lld  avg=%.1f  [us]\n",
           mn, p50, p99, avg);
    printf("  ※ 每次 bm25Score 内部对 %zu 个 term 各调用一次 readPostingList\n",
           query_terms.size());
    printf("    => 单次打分等价于解压 %zu 条完整 posting list\n",
           query_terms.size());
    printf("    => %zu docs × %.1f us/call = %.1f us 总打分开销（占整个 search 的主要部分）\n",
           candidates.size(), avg, candidates.size() * avg);
}

// ─────────────────────────────────────────────────────────────────────────────
// [C] 端到端 search() 延迟
// ─────────────────────────────────────────────────────────────────────────────

struct QueryCase {
    std::string label;
    std::string query;
    ii::QueryMode mode;
};

static void benchSearchLatency(ii::IndexSearcher& searcher,
                                const std::vector<QueryCase>& cases,
                                const Args& args)
{
    printSeparator('=');
    std::cout << "[C] 端到端 search() 延迟\n";
    printSeparator();

    printf("  %-42s  %7s  %9s  %9s  %9s\n",
           "query [mode]", "hits", "p50(us)", "p95(us)", "p99(us)");
    printSeparator();

    for (const auto& qc : cases) {
        // 预热
        for (int w = 0; w < args.warmup; ++w) {
            SilentGuard sg;
            (void)searcher.search(qc.query, args.top_k, qc.mode);
        }

        Stats st;
        int hits = 0;
        for (int r = 0; r < args.repeat; ++r) {
            SilentGuard sg;
            auto t0 = now_us();
            auto res = searcher.search(qc.query, args.top_k, qc.mode);
            auto t1 = now_us();
            st.record(t1 - t0);
            if (r == 0) hits = (int)res.size();
        }

        us_t mn, p50, p95, p99, mx; double avg;
        st.compute(mn, p50, p95, p99, mx, avg);

        std::string label = qc.label + " [" +
                            (qc.mode == ii::QueryMode::AND ? "AND" : "OR") + "]";
        printf("  %-42s  %7d  %9lld  %9lld  %9lld\n",
               label.c_str(), hits, p50, p95, p99);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// [D] 吞吐量（QPS）
// ─────────────────────────────────────────────────────────────────────────────

static void benchThroughput(ii::IndexSearcher& searcher,
                             const std::vector<QueryCase>& cases,
                             const Args& args)
{
    printSeparator('=');
    std::cout << "[D] 吞吐量基准（QPS）\n";
    std::cout << "    将所有查询循环执行，统计持续负载下的 QPS\n";
    printSeparator();

    // 预热
    for (int w = 0; w < args.warmup; ++w)
        for (const auto& qc : cases) {
            SilentGuard sg;
            (void)searcher.search(qc.query, args.top_k, qc.mode);
        }

    const int total_rounds = 1000;
    int total_queries = 0;

    auto t_start = now_us();
    for (int r = 0; r < total_rounds; ++r) {
        for (const auto& qc : cases) {
            SilentGuard sg;
            (void)searcher.search(qc.query, args.top_k, qc.mode);
            ++total_queries;
        }
    }
    auto t_end = now_us();

    double elapsed_s = (t_end - t_start) / 1e6;
    double qps        = total_queries / elapsed_s;
    double avg_us     = (t_end - t_start) / (double)total_queries;

    printf("  总查询数: %d  耗时: %.3fs\n", total_queries, elapsed_s);
    printf("  QPS:     %.0f  avg_latency: %.1f us\n", qps, avg_us);
}

// ─────────────────────────────────────────────────────────────────────────────
// 构造代表性查询集
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<QueryCase> buildQueryCases(
    const std::map<std::string, ii::TermMeta>& dict)
{
    // 从词典中挑选不同 df 层次的 term，用于构造查询
    // 目标：覆盖 单词/双词/三词 × AND/OR

    auto topN = [&](uint32_t lo, uint32_t hi, int n) {
        return pickTerms(dict, lo, hi, n);
    };

    auto high  = topN(100,  UINT32_MAX, 4);  // 高频词
    auto mid   = topN(10,   100,        4);  // 中频词
    auto low   = topN(1,    10,         4);  // 低频词

    auto term = [](const std::vector<std::pair<std::string,uint32_t>>& v, int i) -> std::string {
        return i < (int)v.size() ? v[i].first : "";
    };

    std::vector<QueryCase> cases;

    // 单词查询（高/中/低频）
    if (!high.empty())
        cases.push_back({"1-term high-df[" + term(high,0) + "]", term(high,0), ii::QueryMode::OR});
    if (!mid.empty())
        cases.push_back({"1-term mid-df[" + term(mid,0) + "]",  term(mid,0),  ii::QueryMode::OR});
    if (!low.empty())
        cases.push_back({"1-term low-df[" + term(low,0) + "]",  term(low,0),  ii::QueryMode::OR});

    // 双词查询 AND
    if (high.size() >= 2)
        cases.push_back({"2-term AND high",
                         term(high,0) + " " + term(high,1), ii::QueryMode::AND});
    if (mid.size() >= 2)
        cases.push_back({"2-term AND mid",
                         term(mid,0)  + " " + term(mid,1),  ii::QueryMode::AND});
    if (!low.empty() && !mid.empty())
        cases.push_back({"2-term AND low+mid",
                         term(low,0)  + " " + term(mid,0),  ii::QueryMode::AND});

    // 双词查询 OR
    if (high.size() >= 2)
        cases.push_back({"2-term OR high",
                         term(high,0) + " " + term(high,1), ii::QueryMode::OR});
    if (mid.size() >= 2)
        cases.push_back({"2-term OR mid",
                         term(mid,0)  + " " + term(mid,1),  ii::QueryMode::OR});

    // 三词查询
    if (high.size() >= 3)
        cases.push_back({"3-term OR high",
                         term(high,0) + " " + term(high,1) + " " + term(high,2),
                         ii::QueryMode::OR});
    if (high.size() >= 2 && !low.empty())
        cases.push_back({"3-term AND mixed",
                         term(high,0) + " " + term(mid,0) + " " + term(low,0),
                         ii::QueryMode::AND});

    return cases;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    printSeparator('=', 80);
    printf("  search benchmark  —  baseline (no lazy iterator)\n");
    printf("  index : %s\n", args.index_dir.c_str());
    printf("  warmup: %d   repeat: %d   top_k: %d\n",
           args.warmup, args.repeat, args.top_k);
    printSeparator('=', 80);

    // 静默加载（suppressing SegmentReader 的打印）
    // IndexSearcher 会打印加载信息，这里直接用
    std::cout << "\n";
    ii::IndexSearcher searcher(args.index_dir);

    // 同时打开第一个 segment reader 用于微基准
    // 找第一个 .si 文件
    uint32_t first_seg_id = UINT32_MAX;
    for (const auto& e : fs::directory_iterator(args.index_dir)) {
        auto name = e.path().filename().string();
        if (name.size() > 3 && name[0] == '_' && name.substr(name.size()-3) == ".si") {
            try {
                uint32_t id = std::stoul(name.substr(1, name.size()-4));
                first_seg_id = std::min(first_seg_id, id);
            } catch (...) {}
        }
    }
    if (first_seg_id == UINT32_MAX) {
        std::cerr << "No segments found in " << args.index_dir << "\n";
        return 1;
    }

    ii::SegmentReader seg(args.index_dir, first_seg_id);
    const auto& dict = seg.termDict();

    printf("\n  Segment %u: %u docs, %zu terms\n\n",
           first_seg_id, seg.docCount(), dict.size());

    // ── [A] readPostingList 微基准 ───────────────────────────────────────────
    benchReadPostingList(seg, args);

    // ── [B] bm25Score 冗余开销 ───────────────────────────────────────────────
    printSeparator('=');
    std::cout << "[B] bm25Score 冗余 readPostingList 开销\n";
    std::cout << "    当前 bm25Score 内每个 term 再次全量读 posting，等价于 O(命中docs×term数) 次解压\n";
    printSeparator();

    // 选几个典型查询做 bm25 开销分析
    auto high_terms = pickTerms(dict, 100, UINT32_MAX, 3);
    auto mid_terms  = pickTerms(dict, 10,  100,        3);

    auto buildIdf = [&](const std::vector<std::string>& terms) {
        std::unordered_map<std::string, float> idfs;
        float N = (float)seg.docCount();
        for (const auto& t : terms) {
            const auto* m = seg.getTermMeta(t);
            if (!m) { idfs[t] = 0.0f; continue; }
            float df = (float)m->doc_freq;
            idfs[t] = std::log(1.0f + (N - df + 0.5f) / (df + 0.5f));
        }
        return idfs;
    };

    if (high_terms.size() >= 2) {
        std::vector<std::string> qt = {high_terms[0].first, high_terms[1].first};
        auto idfs = buildIdf(qt);
        benchBm25Overhead(seg, qt, idfs, args);
    }
    if (!high_terms.empty() && !mid_terms.empty()) {
        std::vector<std::string> qt = {high_terms[0].first, mid_terms[0].first};
        auto idfs = buildIdf(qt);
        benchBm25Overhead(seg, qt, idfs, args);
    }

    // ── [C] 端到端 search() 延迟 ─────────────────────────────────────────────
    auto cases = buildQueryCases(dict);
    // 关闭 IndexSearcher 内部打印（无接口，查询时会打印 term 列表）
    // 用 /dev/null redirect 在 shell 层解决，这里不处理
    benchSearchLatency(searcher, cases, args);

    // ── [D] 吞吐量 ───────────────────────────────────────────────────────────
    benchThroughput(searcher, cases, args);

    // ── 总结 ─────────────────────────────────────────────────────────────────
    printSeparator('=');
    std::cout << "  Baseline 总结（lazy iterator 实现后对比此数据）\n";
    printSeparator();
    printf("  关键瓶颈路径（当前）:\n");
    printf("    readPostingList()  : 全量解压，O(df) 内存，高频词每次 > 100us\n");
    printf("    bm25Score()        : 内部对每个 term 再次调 readPostingList\n");
    printf("    searchOR_WAND()    : 构建 cursors 时全量加载所有 term 的 posting list\n");
    printf("\n  lazy iterator 预期改善:\n");
    printf("    内存: O(df) → O(128)  (只保留当前 Block)\n");
    printf("    高频词 readPostingList → advance(): 只解压需要的 Block，跳过已剪枝部分\n");
    printf("    bm25Score 冗余解压: 由 cursor 缓存当前 Block，零重复解压\n");
    printSeparator('=');

    return 0;
}
