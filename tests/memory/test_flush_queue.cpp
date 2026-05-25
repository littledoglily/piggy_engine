// tests/memory/test_flush_queue.cpp
//
// FlushQueue 单元测试
//
// 测试维度：
//   1. push_takebatch_basic   - push N items, takeBatch(N) 返回所有
//   2. partial_batch          - 仅有 1 项时 takeBatch(3) 等待超时后返回 1 项
//   3. shutdown_drains        - shutdown 后队列数据仍可取走，不丢失
//   4. concurrent_push_safe   - 多线程并发 push，总量不丢

#include "memory/flush_queue.h"
#include "memory/frozen_seg_stats.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace ii;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << (msg) << "\n"; assert(false); } while(0)

static FrozenEntry makeEntry(uint32_t seg_id) {
    FrozenEntry e;
    e.segment       = nullptr;
    e.frozen_reader = nullptr;
    e.stats         = std::make_shared<FrozenSegStats>();
    e.stats->seg_id = seg_id;
    return e;
}

// ─── Test 1: push_takebatch_basic ─────────────────────────────────────────────

void test_push_takebatch_basic() {
    TEST(push_takebatch_basic);

    FlushQueue q;
    q.push(makeEntry(1));
    q.push(makeEntry(2));
    q.push(makeEntry(3));

    // 3 items already in queue — should return immediately without waiting timeout
    auto batch = q.takeBatch(3, 500);
    if (batch.size() != 3)
        FAIL("expected 3 items, got " + std::to_string(batch.size()));
    if (batch[0].stats->seg_id != 1 || batch[1].stats->seg_id != 2 || batch[2].stats->seg_id != 3)
        FAIL("FIFO order broken");
    if (q.size() != 0)
        FAIL("queue should be empty after full takeBatch");

    PASS();
}

// ─── Test 2: partial_batch ────────────────────────────────────────────────────

void test_partial_batch() {
    TEST(partial_batch);

    FlushQueue q;
    q.push(makeEntry(42));

    auto start = std::chrono::steady_clock::now();
    // Only 1 item but max_batch=3 — must wait timeout then return the 1 item
    auto batch = q.takeBatch(3, 120);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    if (batch.size() != 1)
        FAIL("expected 1 item after timeout, got " + std::to_string(batch.size()));
    if (batch[0].stats->seg_id != 42)
        FAIL("wrong seg_id in timed-out batch");
    if (elapsed_ms < 80)
        FAIL("returned too fast (" + std::to_string(elapsed_ms) + "ms), expected ~120ms timeout");

    PASS();
}

// ─── Test 3: shutdown_drains ──────────────────────────────────────────────────

void test_shutdown_drains() {
    TEST(shutdown_drains);

    FlushQueue q;
    q.push(makeEntry(10));
    q.push(makeEntry(11));
    q.push(makeEntry(12));
    q.push(makeEntry(13));

    q.shutdown();

    // shutdown() only unblocks waiting takeBatch; queue still readable until empty
    int total = 0;
    while (true) {
        auto batch = q.takeBatch(2, 0);
        if (batch.empty()) break;
        total += static_cast<int>(batch.size());
    }
    if (total != 4)
        FAIL("expected 4 items drained after shutdown, got " + std::to_string(total));

    PASS();
}

// ─── Test 4: concurrent_push_safe ────────────────────────────────────────────

void test_concurrent_push_safe() {
    TEST(concurrent_push_safe);

    FlushQueue q;
    constexpr int N_THREADS    = 6;
    constexpr int N_PER_THREAD = 20;

    std::vector<std::thread> threads;
    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < N_PER_THREAD; ++i)
                q.push(makeEntry(static_cast<uint32_t>(t * 1000 + i)));
        });
    }
    for (auto& th : threads) th.join();

    q.shutdown();

    int total = 0;
    while (true) {
        auto batch = q.takeBatch(10, 0);
        if (batch.empty()) break;
        total += static_cast<int>(batch.size());
    }

    constexpr int expected = N_THREADS * N_PER_THREAD;
    if (total != expected)
        FAIL("expected " + std::to_string(expected) + " items, got " + std::to_string(total));

    PASS();
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== test_flush_queue ===\n";

    test_push_takebatch_basic();
    test_partial_batch();
    test_shutdown_drains();
    test_concurrent_push_safe();

    std::cout << "\nAll tests passed.\n";
    return 0;
}
