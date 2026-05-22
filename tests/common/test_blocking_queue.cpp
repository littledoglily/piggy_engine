// tests/common/test_blocking_queue.cpp
//
// BlockingQueue 单元测试
// 覆盖：
//   1. 基础单线程 push/pop
//   2. close() 让阻塞的 pop 退出
//   3. 关闭后 push 返回 false
//   4. 队列满时生产者阻塞，消费者消费后解除
//   5. MPMC 压力测试：消息无丢失、无重复

#include "common/blocking_queue.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

using namespace ii;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << msg << "\n"; assert(false); } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// 1. 基础单线程 push/pop
// ─────────────────────────────────────────────────────────────────────────────

void test_basic() {
    TEST(basic_push_pop);
    BlockingQueue<int> q(8);

    assert(q.push(1));
    assert(q.push(2));
    assert(q.push(3));
    assert(q.size() == 3);

    int v;
    assert(q.pop(v) && v == 1);
    assert(q.pop(v) && v == 2);
    assert(q.pop(v) && v == 3);
    assert(q.empty());
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. close() 后 pop 排干剩余元素再返回 false
// ─────────────────────────────────────────────────────────────────────────────

void test_close_drains() {
    TEST(close_drains_remaining);
    BlockingQueue<int> q(8);
    q.push(10);
    q.push(20);
    q.close();

    int v;
    // 已有元素仍可取出
    assert(q.pop(v) && v == 10);
    assert(q.pop(v) && v == 20);
    // 空且关闭 → false
    assert(!q.pop(v));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. 关闭后 push 返回 false
// ─────────────────────────────────────────────────────────────────────────────

void test_push_after_close() {
    TEST(push_after_close_returns_false);
    BlockingQueue<int> q(8);
    q.close();
    assert(!q.push(42));
    assert(q.empty());
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. 消费者在空队列上阻塞，close() 后退出
// ─────────────────────────────────────────────────────────────────────────────

void test_blocked_pop_wakes_on_close() {
    TEST(blocked_pop_wakes_on_close);
    BlockingQueue<int> q(8);
    std::atomic<bool> exited{false};

    std::thread consumer([&] {
        int v;
        q.pop(v);       // 会在 close() 前取到 99
        bool got = q.pop(v);  // 空且关闭 → false
        assert(!got);
        exited = true;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    q.push(99);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    q.close();
    consumer.join();

    assert(exited);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. 队列满时生产者阻塞，消费者消费后生产者继续
// ─────────────────────────────────────────────────────────────────────────────

void test_backpressure() {
    TEST(backpressure_on_full_queue);
    const size_t CAP = 4;
    BlockingQueue<int> q(CAP);
    std::atomic<int> produced{0};

    // 先填满
    for (size_t i = 0; i < CAP; ++i) q.push(static_cast<int>(i));

    std::thread producer([&] {
        // 第 CAP+1 个会阻塞
        q.push(999);
        produced = 1;
    });

    // 确保生产者已进入阻塞状态
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    assert(produced == 0);  // 还没写进去

    // 消费一个，解除阻塞
    int v;
    q.pop(v);
    producer.join();
    assert(produced == 1);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. MPMC 压力测试：N 生产者 × M 消费者，消息无丢失无重复
// ─────────────────────────────────────────────────────────────────────────────

void test_mpmc_stress() {
    TEST(mpmc_stress_no_loss_no_dup);
    const int N_PROD    = 4;
    const int N_CONS    = 4;
    const int PER_PROD  = 2000;
    const int TOTAL     = N_PROD * PER_PROD;

    BlockingQueue<int> q(64);
    std::vector<int> received;
    received.reserve(TOTAL);
    std::mutex recv_mu;

    // 消费者
    std::vector<std::thread> cons;
    std::atomic<int> consumed{0};
    for (int c = 0; c < N_CONS; ++c) {
        cons.emplace_back([&] {
            int v;
            while (q.pop(v)) {
                std::lock_guard<std::mutex> lk(recv_mu);
                received.push_back(v);
                consumed.fetch_add(1);
            }
        });
    }

    // 生产者
    std::vector<std::thread> prods;
    for (int p = 0; p < N_PROD; ++p) {
        prods.emplace_back([&, p] {
            for (int i = 0; i < PER_PROD; ++i) {
                q.push(p * PER_PROD + i);
            }
        });
    }

    for (auto& t : prods) t.join();
    q.close();
    for (auto& t : cons) t.join();

    // 验证：数量正确
    if (static_cast<int>(received.size()) != TOTAL)
        FAIL("size mismatch: " + std::to_string(received.size()) + " vs " + std::to_string(TOTAL));

    // 验证：无重复（所有值唯一）
    std::set<int> uniq(received.begin(), received.end());
    if (static_cast<int>(uniq.size()) != TOTAL)
        FAIL("duplicates found");

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== BlockingQueue Tests ===\n";
    test_basic();
    test_close_drains();
    test_push_after_close();
    test_blocked_pop_wakes_on_close();
    test_backpressure();
    test_mpmc_stress();
    std::cout << "All BlockingQueue tests passed.\n";
    return 0;
}
