// tests/common/test_thread_pool.cpp
//
// ThreadPool 单元测试
// 覆盖：
//   1. 提交 N 个任务，全部执行（原子计数器验证）
//   2. waitAll() 在所有任务完成后才返回
//   3. waitAll() 在无任务时立即返回
//   4. 析构时 pending 任务不丢失
//   5. 多次 submit+waitAll 循环可复用

#include "common/thread_pool.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace ii;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << msg << "\n"; assert(false); } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// 1. N 个任务全部执行
// ─────────────────────────────────────────────────────────────────────────────

void test_all_tasks_run() {
    TEST(all_tasks_run);
    const int N = 200;
    std::atomic<int> counter{0};
    {
        ThreadPool pool(4);
        for (int i = 0; i < N; ++i) {
            pool.submit([&] { counter.fetch_add(1); });
        }
    } // 析构 join
    if (counter != N) FAIL("counter=" + std::to_string(counter.load()) + " != " + std::to_string(N));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. waitAll() 等待所有任务完成后才返回
// ─────────────────────────────────────────────────────────────────────────────

void test_wait_all_blocks() {
    TEST(waitAll_blocks_until_done);
    const int N = 50;
    std::atomic<int> counter{0};
    ThreadPool pool(2);

    for (int i = 0; i < N; ++i) {
        pool.submit([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            counter.fetch_add(1);
        });
    }

    pool.waitAll();
    if (counter != N) FAIL("counter=" + std::to_string(counter.load()) + " after waitAll");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. 无任务时 waitAll() 立即返回
// ─────────────────────────────────────────────────────────────────────────────

void test_wait_empty() {
    TEST(waitAll_returns_immediately_when_empty);
    ThreadPool pool(2);
    auto t0 = std::chrono::steady_clock::now();
    pool.waitAll();
    auto elapsed = std::chrono::steady_clock::now() - t0;
    // 应在 50ms 内返回
    if (elapsed > std::chrono::milliseconds(50))
        FAIL("waitAll took too long with no tasks");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. 多次 submit+waitAll 循环
// ─────────────────────────────────────────────────────────────────────────────

void test_reuse() {
    TEST(multiple_submit_waitAll_cycles);
    std::atomic<int> total{0};
    ThreadPool pool(3);

    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 20; ++i) {
            pool.submit([&] { total.fetch_add(1); });
        }
        pool.waitAll();
        // 每轮结束后计数正确
        if (total != (round + 1) * 20)
            FAIL("round " + std::to_string(round) + " total=" + std::to_string(total.load()));
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. 并发提交：多个外部线程同时 submit
// ─────────────────────────────────────────────────────────────────────────────

void test_concurrent_submit() {
    TEST(concurrent_submit_from_multiple_threads);
    const int N_SUBMITTERS = 4;
    const int PER_SUBMITTER = 500;
    std::atomic<int> counter{0};
    ThreadPool pool(4);

    std::vector<std::thread> submitters;
    for (int s = 0; s < N_SUBMITTERS; ++s) {
        submitters.emplace_back([&] {
            for (int i = 0; i < PER_SUBMITTER; ++i) {
                pool.submit([&] { counter.fetch_add(1); });
            }
        });
    }
    for (auto& t : submitters) t.join();
    pool.waitAll();

    int expected = N_SUBMITTERS * PER_SUBMITTER;
    if (counter != expected)
        FAIL("counter=" + std::to_string(counter.load()) + " != " + std::to_string(expected));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== ThreadPool Tests ===\n";
    test_all_tasks_run();
    test_wait_all_blocks();
    test_wait_empty();
    test_reuse();
    test_concurrent_submit();
    std::cout << "All ThreadPool tests passed.\n";
    return 0;
}
