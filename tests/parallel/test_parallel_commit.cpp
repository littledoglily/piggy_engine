// tests/parallel/test_parallel_commit.cpp
//
// Step 4：commit() / close() 机制验证
//
// 核心关注点：worker 在队列关闭后必须把缓冲区中尚未达到 flush 阈值的文档
//   全部 flush 出去（doFlush 在 run() 循环结束后触发），不能丢失。
//
// 验证维度：
//   1. large_threshold_all_buffered：RAM 阈值极大，整个索引过程中 worker 从不
//      自动 flush；commit() 后所有文档仍须正确落盘。
//   2. mixed_threshold：部分文档触发自动 flush，剩余留在缓冲区；commit() 后总数正确。
//   3. empty_commit：无任何文档即 commit()，segmentCount==0，不崩溃。
//   4. commit_idempotency：连续两次 commit() 无副作用，不重复计数。
//   5. many_docs_high_concurrency：大量文档 + 多 worker，总数精确匹配。

#include "index/parallel_index_writer.h"
#include "index/segment_reader.h"
#include "field/schema.h"
#include "core/types.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace ii;
namespace fs = std::filesystem;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << (msg) << "\n"; assert(false); } while(0)

static Schema makeSchema() {
    Schema s;
    s.fields = {
        {"body",     FieldType::Text,    IndexOption::FreqsPositions, true,  false, false, {}},
        {"category", FieldType::Keyword, IndexOption::FreqsOnly,      false, false, false, {}},
    };
    return s;
}

static Document makeDoc(int i) {
    static const std::vector<std::string> bodies = {
        "distributed systems fault tolerance replication",
        "machine learning neural network deep learning",
        "database index query execution plan optimizer",
        "concurrent programming mutex lock condition variable",
        "network socket tcp connection http protocol",
    };
    static const std::vector<std::string> cats = {"sys","ml","db","concurrency","net"};
    Document d;
    d.set("body",     bodies[i % bodies.size()]);
    d.set("category", cats[i % cats.size()]);
    return d;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1：RAM 阈值极大 → worker 全程不自动 flush，全靠 commit() 触发最终 flush
// ─────────────────────────────────────────────────────────────────────────────

void test_large_threshold_all_buffered() {
    TEST(large_threshold_all_buffered_flushed_at_commit);

    const std::string dir = "/tmp/piggy_commit_t1";
    fs::remove_all(dir);

    constexpr int    N        = 120;
    constexpr int    WORKERS  = 4;
    constexpr float  RAM_MB   = 512.f;   // 极大，整个过程不会自动 flush

    {
        ParallelIndexWriter writer(dir, WORKERS, RAM_MB, makeSchema());
        for (int i = 0; i < N; ++i) writer.addDocument(makeDoc(i));
        writer.commit();

        if (writer.totalDocs() != static_cast<uint32_t>(N))
            FAIL("totalDocs=" + std::to_string(writer.totalDocs())
                 + " expected=" + std::to_string(N));

        if (writer.segmentCount() != 1)
            FAIL("segmentCount=" + std::to_string(writer.segmentCount()));
    }

    // SegmentReader 验证
    // （final_seg_id 由 ParallelIndexWriter 内部确定，扫描 .si 找到它）
    uint32_t final_id = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        auto name = e.path().filename().string();
        if (e.is_directory() && name.size() > 8 && name.substr(0, 8) == "segment_" &&
            fs::exists(e.path() / ".done"))
            final_id = static_cast<uint32_t>(std::stoul(name.substr(8)));
    }
    SegmentReader reader(dir, final_id);
    if (reader.docCount() != static_cast<uint32_t>(N))
        FAIL("reader.docCount=" + std::to_string(reader.docCount()));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2：小阈值（部分文档自动 flush）+ 剩余文档靠 commit() flush
// ─────────────────────────────────────────────────────────────────────────────

void test_mixed_auto_and_final_flush() {
    TEST(mixed_auto_flush_and_commit_final_flush);

    const std::string dir = "/tmp/piggy_commit_t2";
    fs::remove_all(dir);

    constexpr int   N       = 200;
    constexpr int   WORKERS = 4;
    constexpr float RAM_MB  = 0.03f;   // 极小，保证中途多次 flush

    {
        ParallelIndexWriter writer(dir, WORKERS, RAM_MB, makeSchema());
        for (int i = 0; i < N; ++i) writer.addDocument(makeDoc(i));
        writer.commit();

        if (writer.totalDocs() != static_cast<uint32_t>(N))
            FAIL("totalDocs=" + std::to_string(writer.totalDocs()));
        if (writer.segmentCount() != 1)
            FAIL("segmentCount=" + std::to_string(writer.segmentCount()));
    }

    uint32_t final_id = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        auto name = e.path().filename().string();
        if (e.is_directory() && name.size() > 8 && name.substr(0, 8) == "segment_" &&
            fs::exists(e.path() / ".done"))
            final_id = static_cast<uint32_t>(std::stoul(name.substr(8)));
    }
    SegmentReader reader(dir, final_id);
    if (reader.docCount() != static_cast<uint32_t>(N))
        FAIL("reader.docCount=" + std::to_string(reader.docCount()));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3：空 commit() — 无文档，不崩溃，segmentCount==0
// ─────────────────────────────────────────────────────────────────────────────

void test_empty_commit() {
    TEST(empty_commit_no_crash_segment_count_zero);

    const std::string dir = "/tmp/piggy_commit_t3";
    fs::remove_all(dir);

    {
        ParallelIndexWriter writer(dir, 4, 64.f, makeSchema());
        writer.commit();

        if (writer.totalDocs() != 0)
            FAIL("totalDocs=" + std::to_string(writer.totalDocs()));
        if (writer.segmentCount() != 0)
            FAIL("segmentCount=" + std::to_string(writer.segmentCount()));
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4：连续两次 commit() — 第二次是 no-op，文档不重复计数
// ─────────────────────────────────────────────────────────────────────────────

void test_commit_idempotency() {
    TEST(commit_idempotency_no_double_count);

    const std::string dir = "/tmp/piggy_commit_t4";
    fs::remove_all(dir);

    constexpr int N = 80;

    {
        ParallelIndexWriter writer(dir, 4, 64.f, makeSchema());
        for (int i = 0; i < N; ++i) writer.addDocument(makeDoc(i));
        writer.commit();
        writer.commit();   // 第二次 commit()：no-op

        if (writer.totalDocs() != static_cast<uint32_t>(N))
            FAIL("totalDocs after double commit=" + std::to_string(writer.totalDocs()));
        if (writer.segmentCount() != 1)
            FAIL("segmentCount=" + std::to_string(writer.segmentCount()));
    }

    // 用 SegmentReader 确认文档数不重复
    uint32_t final_id = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        auto name = e.path().filename().string();
        if (e.is_directory() && name.size() > 8 && name.substr(0, 8) == "segment_" &&
            fs::exists(e.path() / ".done"))
            final_id = static_cast<uint32_t>(std::stoul(name.substr(8)));
    }
    SegmentReader reader(dir, final_id);
    if (reader.docCount() != static_cast<uint32_t>(N))
        FAIL("reader.docCount=" + std::to_string(reader.docCount())
             + " expected=" + std::to_string(N));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5：大批量 + 多 worker，精确总数
// ─────────────────────────────────────────────────────────────────────────────

void test_high_concurrency_no_loss() {
    TEST(high_concurrency_no_doc_loss);

    const std::string dir = "/tmp/piggy_commit_t5";
    fs::remove_all(dir);

    constexpr int   N       = 1000;
    constexpr int   WORKERS = 8;
    constexpr float RAM_MB  = 0.05f;

    uint32_t final_id = 0;
    {
        ParallelIndexWriter writer(dir, WORKERS, RAM_MB, makeSchema());
        for (int i = 0; i < N; ++i) writer.addDocument(makeDoc(i));
        writer.commit();

        if (writer.totalDocs() != static_cast<uint32_t>(N))
            FAIL("totalDocs=" + std::to_string(writer.totalDocs()));

        final_id = writer.segmentIds().front();
    }

    SegmentReader reader(dir, final_id);
    if (reader.docCount() != static_cast<uint32_t>(N))
        FAIL("reader.docCount=" + std::to_string(reader.docCount()));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== Parallel Commit / close() Tests (Step 4) ===\n\n";

    test_large_threshold_all_buffered();
    test_mixed_auto_and_final_flush();
    test_empty_commit();
    test_commit_idempotency();
    test_high_concurrency_no_loss();

    std::cout << "\nAll Step 4 tests passed.\n";
    return 0;
}
