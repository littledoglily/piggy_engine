// tests/memory/test_flush_worker.cpp
//
// FlushWorker 单元测试
//
// 测试维度：
//   1. single_flush            - 1 个 FrozenEntry 落盘后，磁盘 reader 可查
//   2. three_entry_kway_merge  - 3 个 FrozenEntry 合并为 1 个 segment，各自 term 均可查
//   3. stop_exits_cleanly      - 空队列下 stop() 迅速返回，无 hang

#include "memory/flush_worker.h"
#include "memory/flush_queue.h"
#include "memory/frozen_seg_stats.h"
#include "memory/memory_segment.h"
#include "memory/memory_segment_reader.h"
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
using namespace ii::memory;
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

// Build a FrozenEntry from a shared MemorySegment and register the frozen reader with searcher.
static FrozenEntry buildEntry(std::shared_ptr<MemorySegment> ms,
                               IndexSearcher&                 searcher,
                               uint32_t                       seg_id) {
    auto frozen_reader = std::make_shared<MemorySegmentReader>(
        std::static_pointer_cast<const MemorySegment>(ms));
    searcher.addReader(frozen_reader);

    FrozenEntry e;
    e.segment       = ms;
    e.frozen_reader = frozen_reader;
    e.stats         = std::make_shared<FrozenSegStats>();
    e.stats->seg_id = seg_id;
    e.stats->state  = FrozenSegState::Queued;
    return e;
}

// ─── Test 1: single_flush ─────────────────────────────────────────────────────

void test_single_flush() {
    TEST(single_flush);

    TempDir td("/tmp/piggy_fw_single");
    Schema  schema = makeSchema();
    FlushQueue queue;
    std::atomic<uint32_t> global_seg_id{100};
    IndexSearcher searcher(td.path);
    FlushWorker worker(td.path, schema, queue, searcher, global_seg_id);
    worker.start();

    // Build a segment with docs containing "zorblax"
    auto ms = std::make_shared<MemorySegment>(schema);
    Document d1; d1.set("body", "zorblax unique term alpha");
    Document d2; d2.set("body", "zorblax unique term beta");
    ms->addDocument(d1, 1);
    ms->addDocument(d2, 2);

    queue.push(buildEntry(ms, searcher, 100));

    worker.stop();  // drains queue; blocks until all processBatch calls complete

    auto results = searcher.search("body:zorblax", 10, QueryMode::OR);
    if (results.size() != 2)
        FAIL("expected 2 hits for 'zorblax', got " + std::to_string(results.size()));

    PASS();
}

// ─── Test 2: three_entry_kway_merge ──────────────────────────────────────────

void test_three_entry_kway_merge() {
    TEST(three_entry_kway_merge);

    TempDir td("/tmp/piggy_fw_kway");
    Schema  schema = makeSchema();
    FlushQueue queue;
    std::atomic<uint32_t> global_seg_id{200};
    IndexSearcher searcher(td.path);
    FlushWorker worker(td.path, schema, queue, searcher, global_seg_id);

    // Prepare 3 segments with distinct unique terms
    const std::vector<std::string> unique_terms = {"frobnik", "quuxberry", "ziltoid"};
    for (size_t i = 0; i < 3; ++i) {
        auto ms = std::make_shared<MemorySegment>(schema);
        Document d;
        d.set("body", unique_terms[i] + " common content search");
        ms->addDocument(d, 1);
        queue.push(buildEntry(ms, searcher, static_cast<uint32_t>(200 + i)));
    }

    // Push all 3 before start so they're batch-taken together (size >= 3 triggers immediately)
    worker.start();
    worker.stop();

    // All 3 unique terms should be findable after k-way merge
    for (const auto& term : unique_terms) {
        auto results = searcher.search("body:" + term, 10, QueryMode::OR);
        if (results.empty())
            FAIL("expected hit for '" + term + "' after kway merge, got 0");
    }

    PASS();
}

// ─── Test 3: stop_exits_cleanly ──────────────────────────────────────────────

void test_stop_exits_cleanly() {
    TEST(stop_exits_cleanly);

    TempDir td("/tmp/piggy_fw_stop");
    Schema  schema = makeSchema();
    FlushQueue queue;
    std::atomic<uint32_t> global_seg_id{300};
    IndexSearcher searcher(td.path);
    FlushWorker worker(td.path, schema, queue, searcher, global_seg_id);
    worker.start();

    auto t0 = std::chrono::steady_clock::now();
    worker.stop();  // empty queue — should return promptly (within ~600ms of takeBatch timeout)
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // The worker polls every 500ms; stop should terminate within ~600ms
    if (elapsed_ms > 2000)
        FAIL("stop() took too long: " + std::to_string(elapsed_ms) + "ms");

    PASS();
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== test_flush_worker ===\n";

    test_single_flush();
    test_three_entry_kway_merge();
    test_stop_exits_cleanly();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
