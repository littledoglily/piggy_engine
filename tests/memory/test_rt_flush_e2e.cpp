// tests/memory/test_rt_flush_e2e.cpp
//
// 实时索引端到端测试（RealtimeIndexManager）
//
// 测试维度：
//   1. e2e_sequential_write       - 单线程写入，stop() 后全部文档可查
//   2. e2e_concurrent_write       - N 个写线程并发写，stop() 后全部文档可查
//   3. e2e_ext_id_dedup           - 重复 ext_id 的文档在搜索结果中只出现一次
//   4. e2e_no_visibility_gap      - 写入期间搜索不返回 0 hit（一旦有文档写入即可见）

#include "memory/realtime_index_manager.h"
#include "memory/flush_queue.h"
#include "query/index_searcher.h"
#include "field/schema.h"
#include "core/types.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace ii;
namespace fs = std::filesystem;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << (msg) << "\n"; assert(false); } while(0)

struct TempDir {
    std::string path;
    explicit TempDir(const std::string& p) : path(p) { fs::create_directories(p); }
    ~TempDir() { fs::remove_all(path); }
};

static Schema makeSchema() {
    Schema s;
    s.fields = {{"body", FieldType::Text, IndexOption::FreqsPositions, false, false, false, {}}};
    return s;
}

// ─── Test 1: e2e_sequential_write ────────────────────────────────────────────

void test_e2e_sequential_write() {
    TEST(e2e_sequential_write);

    TempDir td("/tmp/piggy_e2e_seq");
    Schema schema = makeSchema();
    IndexSearcher searcher(td.path);

    // Small ram threshold to trigger frequent flushes (4 KB)
    FreezeConfig cfg{4 * 1024, 0.99f};
    RealtimeIndexManager mgr(td.path, schema, searcher, 1, cfg, 2000);
    mgr.start();

    constexpr int N = 20;
    for (int i = 0; i < N; ++i) {
        Document d;
        d.ext_id = static_cast<uint64_t>(i + 1);
        d.set("body", "sequentialterm document index " + std::to_string(i));
        mgr.addDocument(d);
    }

    mgr.stop();  // flush all remaining, wait for FlushWorker to commit

    auto results = searcher.search("body:sequentialterm", 100, QueryMode::OR);
    if (static_cast<int>(results.size()) != N)
        FAIL("expected " + std::to_string(N) + " hits, got " + std::to_string(results.size()));

    PASS();
}

// ─── Test 2: e2e_concurrent_write ────────────────────────────────────────────

void test_e2e_concurrent_write() {
    TEST(e2e_concurrent_write);

    TempDir td("/tmp/piggy_e2e_conc");
    Schema schema = makeSchema();
    IndexSearcher searcher(td.path);

    FreezeConfig cfg{4 * 1024, 0.99f};
    constexpr int N_RT_THREADS = 3;
    RealtimeIndexManager mgr(td.path, schema, searcher, N_RT_THREADS, cfg, 3000);
    mgr.start();

    constexpr int N_WRITE_THREADS = 4;
    constexpr int N_PER_THREAD    = 15;
    std::atomic<uint64_t> ext_id_counter{1};

    std::vector<std::thread> writers;
    for (int t = 0; t < N_WRITE_THREADS; ++t) {
        writers.emplace_back([&]() {
            for (int i = 0; i < N_PER_THREAD; ++i) {
                Document d;
                d.ext_id = ext_id_counter.fetch_add(1);
                d.set("body", "concurrentterm thread content " + std::to_string(d.ext_id));
                mgr.addDocument(d);
            }
        });
    }
    for (auto& w : writers) w.join();

    mgr.stop();

    constexpr int total = N_WRITE_THREADS * N_PER_THREAD;
    auto results = searcher.search("body:concurrentterm", total + 10, QueryMode::OR);
    if (static_cast<int>(results.size()) != total)
        FAIL("expected " + std::to_string(total) + " hits, got " + std::to_string(results.size()));

    PASS();
}

// ─── Test 3: e2e_ext_id_dedup ────────────────────────────────────────────────

void test_e2e_ext_id_dedup() {
    TEST(e2e_ext_id_dedup);

    TempDir td("/tmp/piggy_e2e_dedup");
    Schema schema = makeSchema();
    IndexSearcher searcher(td.path);

    // Large thresholds so docs won't auto-freeze; we'll flush via stop()
    FreezeConfig cfg{256ULL * 1024 * 1024, 0.99f};
    // Use 2 RT threads so the same ext_id might end up in different readers
    RealtimeIndexManager mgr(td.path, schema, searcher, 2, cfg, 4000);
    mgr.start();

    // Write 3 different ext_ids, but one of them appears twice (simulating an upsert)
    constexpr uint64_t DUPE_EXT_ID = 999;
    Document d1; d1.ext_id = 1;          d1.set("body", "dupterm first doc");
    Document d2; d2.ext_id = DUPE_EXT_ID; d2.set("body", "dupterm second doc version one");
    Document d3; d3.ext_id = DUPE_EXT_ID; d3.set("body", "dupterm second doc version two");
    Document d4; d4.ext_id = 3;          d4.set("body", "dupterm fourth doc");

    mgr.addDocument(d1);
    mgr.addDocument(d2);
    mgr.addDocument(d3);
    mgr.addDocument(d4);

    mgr.stop();

    // Search for "dupterm": should get 3 unique ext_ids (1, 999, 3) — ext_id 999 deduplicated
    auto results = searcher.search("body:dupterm", 20, QueryMode::OR);
    if (results.size() != 3)
        FAIL("expected 3 deduplicated hits (ext_id 1/999/3), got " +
             std::to_string(results.size()));

    // Verify DUPE_EXT_ID appears exactly once
    int dupe_count = 0;
    for (auto& r : results)
        if (r.ext_id == DUPE_EXT_ID) ++dupe_count;
    if (dupe_count != 1)
        FAIL("ext_id 999 should appear exactly once, got " + std::to_string(dupe_count));

    PASS();
}

// ─── Test 4: e2e_no_visibility_gap ───────────────────────────────────────────
//
// 写入过程中并发搜索：一旦至少有 1 篇文档写入，搜索就应可见。
// 验证：完成所有写入后，最终结果全部可见。

void test_e2e_no_visibility_gap() {
    TEST(e2e_no_visibility_gap);

    TempDir td("/tmp/piggy_e2e_gap");
    Schema schema = makeSchema();
    IndexSearcher searcher(td.path);

    FreezeConfig cfg{4 * 1024, 0.99f};
    RealtimeIndexManager mgr(td.path, schema, searcher, 2, cfg, 5000);
    mgr.start();

    constexpr int TOTAL_DOCS = 30;
    std::atomic<bool> done{false};
    std::atomic<int>  max_seen{0};

    // Reader thread: continuously searches and tracks max hits seen
    std::thread reader([&]() {
        while (!done.load(std::memory_order_acquire)) {
            auto results = searcher.search("body:gapterm", TOTAL_DOCS + 10, QueryMode::OR);
            int n = static_cast<int>(results.size());
            int prev = max_seen.load(std::memory_order_relaxed);
            while (n > prev && !max_seen.compare_exchange_weak(prev, n)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Writer thread: write all docs
    for (int i = 0; i < TOTAL_DOCS; ++i) {
        Document d;
        d.ext_id = static_cast<uint64_t>(i + 1);
        d.set("body", "gapterm visibility content " + std::to_string(i));
        mgr.addDocument(d);
    }

    mgr.stop();  // all docs committed to disk before we stop the reader

    done.store(true, std::memory_order_release);
    reader.join();

    // Final search: all docs must be visible after stop()
    auto final_results = searcher.search("body:gapterm", TOTAL_DOCS + 10, QueryMode::OR);
    if (static_cast<int>(final_results.size()) != TOTAL_DOCS)
        FAIL("expected " + std::to_string(TOTAL_DOCS) +
             " hits after stop(), got " + std::to_string(final_results.size()));

    // During writes, reader should have seen at least some hits (visibility during writing)
    if (max_seen.load() == 0)
        FAIL("reader saw 0 hits during write window — visibility gap detected");

    PASS();
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== test_rt_flush_e2e ===\n";

    test_e2e_sequential_write();
    test_e2e_concurrent_write();
    test_e2e_ext_id_dedup();
    test_e2e_no_visibility_gap();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
