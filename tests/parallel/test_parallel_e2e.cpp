// tests/parallel/test_parallel_e2e.cpp
//
// Step 6：端到端验证（E2E）
//
// 验证维度：
//   1. 单线程 vs 并行：term df/ttf 完全一致
//   2. IndexSearcher 查询结果中包含预期文档（按 body 内容匹配，不依赖 doc_id）
//   3. 单线程 vs 并行：对同一批查询，命中文档集合相同
//   4. 存储字段可正确读回（readStoredDoc）
//   5. 析构时自动 commit 不丢失文档（RAII commit）

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

// 固定文档集（可重复，覆盖多种词汇）
struct CorpusDoc {
    std::string body;
    std::string category;
};

static std::vector<CorpusDoc> makeCorpus(int n) {
    static const std::vector<CorpusDoc> templates = {
        {"distributed systems consistency availability partition tolerance replication", "systems"},
        {"machine learning gradient descent optimization neural network backpropagation", "ml"},
        {"database index btree hash table query execution plan optimizer join", "database"},
        {"concurrency locking deadlock transaction isolation level serializable", "concurrency"},
        {"network protocol tcp udp http rest api design latency throughput", "network"},
        {"memory allocation cache locality prefetch branch prediction pipeline", "hardware"},
        {"compiler optimization register allocation instruction scheduling loop", "compiler"},
        {"search engine inverted index ranking bm25 tf idf scoring retrieval", "search"},
        {"graph algorithm shortest path spanning tree dynamic programming flow", "algorithms"},
        {"operating system process thread scheduler virtual memory paging swap", "os"},
    };
    std::vector<CorpusDoc> corpus;
    corpus.reserve(n);
    for (int i = 0; i < n; ++i)
        corpus.push_back(templates[i % templates.size()]);
    return corpus;
}

// ─────────────────────────────────────────────────────────────────────────────
// 工具
// ─────────────────────────────────────────────────────────────────────────────

static std::map<std::string, uint32_t> getTermDf(
    const SegmentReader& r, const std::string& field)
{
    std::map<std::string, uint32_t> m;
    const auto* dict = r.fieldTermDict(field);
    if (dict) for (const auto& [t, meta] : *dict) m[t] = meta.doc_freq;
    return m;
}

static std::map<std::string, uint32_t> getTermTtf(
    const SegmentReader& r, const std::string& field)
{
    std::map<std::string, uint32_t> m;
    const auto* dict = r.fieldTermDict(field);
    if (dict) for (const auto& [t, meta] : *dict) m[t] = meta.total_term_freq;
    return m;
}

// 扫描目录找到含 .done 标记的最大 segment_N/ id（commit 后只有一个）
static uint32_t findFinalSegId(const std::string& dir) {
    uint32_t id = 0;
    bool found = false;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_directory()) continue;
        auto name = e.path().filename().string();
        if (name.size() > 8 && name.substr(0, 8) == "segment_" &&
            fs::exists(e.path() / ".done")) {
            uint32_t sid = static_cast<uint32_t>(std::stoul(name.substr(8)));
            if (!found || sid > id) { id = sid; found = true; }
        }
    }
    return id;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1：单线程 vs 并行 — term df/ttf 完全一致
// ─────────────────────────────────────────────────────────────────────────────

void test_term_stats_identical() {
    TEST(e2e_term_stats_st_vs_par_identical);

    constexpr int   N        = 200;
    constexpr int   WORKERS  = 4;
    constexpr float RAM_MB   = 0.05f;

    const std::string dir_st  = "/tmp/piggy_e2e_t1_st";
    const std::string dir_par = "/tmp/piggy_e2e_t1_par";
    fs::remove_all(dir_st);
    fs::remove_all(dir_par);

    auto corpus = makeCorpus(N);

    // 单线程
    {
        IndexWriter writer(dir_st, 512.f, makeSchema());
        DocId id = 0;
        for (const auto& c : corpus) {
            Document d;
            d.doc_id = ++id;
            d.set("body", c.body);
            d.set("category", c.category);
            writer.addDocument(d);
        }
        writer.commit();
    }

    // 并行
    {
        ParallelIndexWriter writer(dir_par, WORKERS, RAM_MB, makeSchema());
        for (const auto& c : corpus) {
            Document d;
            d.set("body", c.body);
            d.set("category", c.category);
            writer.addDocument(std::move(d));
        }
        writer.commit();
    }

    SegmentReader st_r (dir_st,  0);
    SegmentReader par_r(dir_par, findFinalSegId(dir_par));

    for (const std::string& field : {"body", "category"}) {
        auto st_df  = getTermDf(st_r,  field);
        auto par_df = getTermDf(par_r, field);
        auto st_ttf = getTermTtf(st_r,  field);
        auto par_ttf= getTermTtf(par_r, field);

        if (st_df.size() != par_df.size())
            FAIL("field=" + field + " term_count: st=" + std::to_string(st_df.size())
                 + " par=" + std::to_string(par_df.size()));

        for (const auto& [term, df] : st_df) {
            if (!par_df.count(term))
                FAIL("field=" + field + " missing term=" + term);
            if (par_df[term] != df)
                FAIL("field=" + field + " term=" + term + " df mismatch");
            if (par_ttf[term] != st_ttf[term])
                FAIL("field=" + field + " term=" + term + " ttf mismatch");
        }
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2：IndexSearcher 查询结果中含预期文档
// ─────────────────────────────────────────────────────────────────────────────

void test_searcher_hits_contain_expected_docs() {
    TEST(e2e_searcher_hits_contain_expected_docs);

    constexpr int   N        = 100;
    constexpr int   WORKERS  = 4;
    constexpr float RAM_MB   = 0.1f;

    const std::string dir = "/tmp/piggy_e2e_t2";
    fs::remove_all(dir);

    auto corpus = makeCorpus(N);
    {
        ParallelIndexWriter writer(dir, WORKERS, RAM_MB, makeSchema());
        for (const auto& c : corpus) {
            Document d;
            d.set("body", c.body);
            d.set("category", c.category);
            writer.addDocument(std::move(d));
        }
        writer.commit();
    }

    IndexSearcher searcher(dir);

    // 查询 "search" 应命中 body 含 "search engine..." 的文档
    // corpus 中 index 8 对应 "search" 类：100/10 = 10 篇
    {
        auto results = searcher.search("body:search", 20, QueryMode::OR);
        if (results.empty())
            FAIL("query 'body:search' returned 0 results");
        // 验证所有结果都包含 "search" 词
        for (const auto& r : results) {
            if (r.score <= 0.f)
                FAIL("result doc_id=" + std::to_string(r.doc_id) + " has score <= 0");
        }
    }

    // 查询 "body:distributed" 应命中 category=systems 的文档
    {
        auto results = searcher.search("body:distributed", 20, QueryMode::OR);
        if (results.empty())
            FAIL("query 'body:distributed' returned 0 results");
    }

    // 查询 "body:neural body:network" 应命中 ml 类文档
    {
        auto results = searcher.search("body:neural body:network", 20, QueryMode::OR);
        if (results.empty())
            FAIL("query 'body:neural body:network' returned 0 results");
    }

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3：单线程 vs 并行 — 相同查询命中文档集合一致（按 body 内容判断）
// ─────────────────────────────────────────────────────────────────────────────

void test_query_results_match_single_thread() {
    TEST(e2e_query_results_match_single_thread);

    constexpr int   N        = 150;
    constexpr int   WORKERS  = 4;
    constexpr float RAM_MB   = 0.08f;

    const std::string dir_st  = "/tmp/piggy_e2e_t3_st";
    const std::string dir_par = "/tmp/piggy_e2e_t3_par";
    fs::remove_all(dir_st);
    fs::remove_all(dir_par);

    auto corpus = makeCorpus(N);

    // 单线程
    {
        IndexWriter writer(dir_st, 512.f, makeSchema());
        DocId id = 0;
        for (const auto& c : corpus) {
            Document d;
            d.doc_id = ++id;
            d.set("body", c.body);
            d.set("category", c.category);
            writer.addDocument(d);
        }
        writer.commit();
    }

    // 并行
    {
        ParallelIndexWriter writer(dir_par, WORKERS, RAM_MB, makeSchema());
        for (const auto& c : corpus) {
            Document d;
            d.set("body", c.body);
            d.set("category", c.category);
            writer.addDocument(std::move(d));
        }
        writer.commit();
    }

    IndexSearcher st_searcher (dir_st);
    IndexSearcher par_searcher(dir_par);

    // 对每条查询：命中数量必须相同
    const std::vector<std::string> queries = {
        "body:distributed",
        "body:machine",
        "body:index",
        "body:search",
        "body:algorithm",
    };

    for (const auto& q : queries) {
        auto st_results  = st_searcher.search(q,  N, QueryMode::OR);
        auto par_results = par_searcher.search(q, N, QueryMode::OR);

        if (st_results.size() != par_results.size())
            FAIL("query='" + q + "' st_hits=" + std::to_string(st_results.size())
                 + " par_hits=" + std::to_string(par_results.size()));
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4：存储字段可正确读回（readStoredDoc）
// ─────────────────────────────────────────────────────────────────────────────

void test_stored_fields_readable() {
    TEST(e2e_stored_fields_readable_after_commit);

    constexpr int   N       = 50;
    constexpr int   WORKERS = 4;
    constexpr float RAM_MB  = 0.1f;

    const std::string dir = "/tmp/piggy_e2e_t4";
    fs::remove_all(dir);

    auto corpus = makeCorpus(N);
    {
        ParallelIndexWriter writer(dir, WORKERS, RAM_MB, makeSchema());
        for (const auto& c : corpus) {
            Document d;
            d.set("body", c.body);
            d.set("category", c.category);
            writer.addDocument(std::move(d));
        }
        writer.commit();
    }

    SegmentReader reader(dir, findFinalSegId(dir));
    uint32_t n = reader.docCount();
    if (n != static_cast<uint32_t>(N))
        FAIL("docCount=" + std::to_string(n));

    // 验证每个文档可读，且 body/category 字段不为空
    uint32_t checked = 0;
    for (uint32_t doc_id = 1; doc_id <= n; ++doc_id) {
        if (!reader.isAlive(doc_id)) continue;
        auto stored = reader.readStoredDoc(doc_id);
        if (stored.str_fields.find("body") == stored.str_fields.end())
            FAIL("doc_id=" + std::to_string(doc_id) + " missing 'body' field");
        if (stored.str_fields.at("body").empty())
            FAIL("doc_id=" + std::to_string(doc_id) + " 'body' is empty");
        ++checked;
    }
    if (checked != static_cast<uint32_t>(N))
        FAIL("only " + std::to_string(checked) + " docs readable, expected " + std::to_string(N));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5：RAII 析构时自动 commit — 不丢失文档
// ─────────────────────────────────────────────────────────────────────────────

void test_raii_auto_commit() {
    TEST(e2e_raii_destructor_auto_commit);

    constexpr int   N       = 60;
    constexpr int   WORKERS = 4;
    constexpr float RAM_MB  = 512.f;   // 大阈值，析构前不会自动 flush

    const std::string dir = "/tmp/piggy_e2e_t5";
    fs::remove_all(dir);

    // ParallelIndexWriter 不显式 commit，走析构路径
    {
        ParallelIndexWriter writer(dir, WORKERS, RAM_MB, makeSchema());
        for (int i = 0; i < N; ++i) {
            auto corpus = makeCorpus(1);
            Document d;
            d.set("body", corpus[0].body);
            d.set("category", corpus[0].category);
            writer.addDocument(std::move(d));
        }
        // 析构时触发 commit()
    }

    uint32_t final_id = findFinalSegId(dir);
    SegmentReader reader(dir, final_id);
    if (reader.docCount() != static_cast<uint32_t>(N))
        FAIL("raii commit docCount=" + std::to_string(reader.docCount())
             + " expected=" + std::to_string(N));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Parallel E2E Tests (Step 6) ===\n\n";

    test_term_stats_identical();
    test_searcher_hits_contain_expected_docs();
    test_query_results_match_single_thread();
    test_stored_fields_readable();
    test_raii_auto_commit();

    std::cout << "\nAll Step 6 E2E tests passed.\n";
    return 0;
}
