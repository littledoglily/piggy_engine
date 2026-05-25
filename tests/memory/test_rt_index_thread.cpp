// tests/memory/test_rt_index_thread.cpp
//
// RTIndexThread 单元测试
//
// 测试维度：
//   1. freeze_on_ram_threshold  - ram 阈值极小，首次 addDocument 后即触发 freeze，
//                                  旧 frozen reader 留在 all_readers_，新段可继续写
//   2. post_freeze_searchable   - freeze 后旧段的文档在 IndexSearcher 中仍可检索到
//   3. new_segment_writable     - freeze 后新段接收新文档，搜索可见
//   4. shutdown_flushes_remaining - shutdown() 强制 freeze 当前活跃段进入 FlushQueue

#include "memory/rt_index_thread.h"
#include "memory/flush_queue.h"
#include "memory/frozen_seg_stats.h"
#include "query/index_searcher.h"
#include "field/schema.h"
#include "core/types.h"

#include <atomic>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

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

// FreezeConfig that triggers freeze after first addDocument (1 byte RAM threshold)
static FreezeConfig tinyConfig() {
    return FreezeConfig{1, 2.0f};  // ram_bytes_threshold=1 → always freeze; hash_utilization=2 (unreachable)
}

// FreezeConfig that never triggers naturally (large thresholds)
static FreezeConfig largeConfig() {
    return FreezeConfig{256ULL * 1024 * 1024, 0.99f};
}

// ─── Test 1: freeze_on_ram_threshold ─────────────────────────────────────────

void test_freeze_on_ram_threshold() {
    TEST(freeze_on_ram_threshold);

    TempDir td("/tmp/piggy_rit_freeze");
    Schema schema = makeSchema();
    FlushQueue queue;
    std::atomic<uint32_t> global_seg_id{1000};
    IndexSearcher searcher(td.path);

    RTIndexThread rt(0, td.path, schema, searcher, queue, global_seg_id, tinyConfig());

    // Before any document: active reader registered (0 docs), queue empty
    if (queue.size() != 0)
        FAIL("queue should be empty before first addDocument");

    Document d;
    d.set("body", "freeze trigger document");
    rt.addDocument(d);

    // tinyConfig: ramUsed >= 1 is true after first addDocument → freeze triggered
    if (queue.size() != 1)
        FAIL("expected 1 FrozenEntry in queue after freeze, got " + std::to_string(queue.size()));

    rt.shutdown();  // new active segment has 0 docs → removeReader, no extra queue entry

    PASS();
}

// ─── Test 2: post_freeze_searchable ──────────────────────────────────────────

void test_post_freeze_searchable() {
    TEST(post_freeze_searchable);

    TempDir td("/tmp/piggy_rit_searchable");
    Schema schema = makeSchema();
    FlushQueue queue;
    std::atomic<uint32_t> global_seg_id{1100};
    IndexSearcher searcher(td.path);

    // Use large thresholds so we can control freeze manually via shutdown()
    FreezeConfig cfg = largeConfig();
    RTIndexThread rt(0, td.path, schema, searcher, queue, global_seg_id, cfg);

    // Write docs with a unique term
    for (int i = 1; i <= 5; ++i) {
        Document d;
        d.set("body", "xyzterm content doc" + std::to_string(i));
        rt.addDocument(d);
    }

    // Force freeze by shrinking threshold: can't easily change cfg after construction.
    // Instead simulate freeze scenario via tinyConfig from the start:
    // Re-run with tinyConfig to test searchability after auto-freeze.

    // This test creates a fresh rt with tinyConfig to trigger freeze automatically.
    FlushQueue queue2;
    std::atomic<uint32_t> global_seg_id2{1200};
    IndexSearcher searcher2(td.path);
    RTIndexThread rt2(0, td.path, schema, searcher2, queue2, global_seg_id2, tinyConfig());

    Document d1; d1.set("body", "uniqueterm alpha post freeze");
    rt2.addDocument(d1);
    // tinyConfig → freeze happens; frozen reader stays in all_readers_

    // Should still be searchable via the frozen reader in all_readers_
    auto results = searcher2.search("body:uniqueterm", 10, QueryMode::OR);
    if (results.empty())
        FAIL("expected 'uniqueterm' to be searchable after freeze, got 0 hits");

    rt2.shutdown();

    PASS();
}

// ─── Test 3: new_segment_writable ────────────────────────────────────────────

void test_new_segment_writable() {
    TEST(new_segment_writable);

    TempDir td("/tmp/piggy_rit_newseq");
    Schema schema = makeSchema();
    FlushQueue queue;
    std::atomic<uint32_t> global_seg_id{1300};
    IndexSearcher searcher(td.path);

    RTIndexThread rt(0, td.path, schema, searcher, queue, global_seg_id, tinyConfig());

    // First doc → triggers freeze; "pregap" lives in frozen reader
    Document d1; d1.set("body", "pregap document old segment");
    rt.addDocument(d1);

    // Second doc → goes to new active segment; also triggers freeze (tinyConfig)
    Document d2; d2.set("body", "newterm fresh new segment");
    rt.addDocument(d2);

    // Both terms should be searchable (old via frozen reader, new via newer frozen reader)
    auto r1 = searcher.search("body:pregap",  10, QueryMode::OR);
    auto r2 = searcher.search("body:newterm", 10, QueryMode::OR);
    if (r1.empty())
        FAIL("pregap should be searchable from first frozen reader");
    if (r2.empty())
        FAIL("newterm should be searchable from second frozen reader");

    rt.shutdown();

    PASS();
}

// ─── Test 4: shutdown_flushes_remaining ──────────────────────────────────────

void test_shutdown_flushes_remaining() {
    TEST(shutdown_flushes_remaining);

    TempDir td("/tmp/piggy_rit_shutdown");
    Schema schema = makeSchema();
    FlushQueue queue;
    std::atomic<uint32_t> global_seg_id{1400};
    IndexSearcher searcher(td.path);

    // Large thresholds → no natural freeze; docs accumulate until shutdown
    RTIndexThread rt(0, td.path, schema, searcher, queue, global_seg_id, largeConfig());

    for (int i = 1; i <= 10; ++i) {
        Document d;
        d.set("body", "accumulated document index " + std::to_string(i));
        rt.addDocument(d);
    }

    if (queue.size() != 0)
        FAIL("expected 0 queue entries before shutdown (no natural freeze)");

    rt.shutdown();  // forces freeze → 1 entry pushed to queue

    if (queue.size() != 1)
        FAIL("expected 1 FrozenEntry in queue after shutdown, got " + std::to_string(queue.size()));

    // Drain and verify the entry has the right doc count
    queue.shutdown();
    auto batch = queue.takeBatch(10, 0);
    if (batch.size() != 1)
        FAIL("expected 1 entry in drain batch");
    if (batch[0].stats->doc_count != 10)
        FAIL("expected doc_count=10, got " + std::to_string(batch[0].stats->doc_count));

    PASS();
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== test_rt_index_thread ===\n";

    test_freeze_on_ram_threshold();
    test_post_freeze_searchable();
    test_new_segment_writable();
    test_shutdown_flushes_remaining();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
