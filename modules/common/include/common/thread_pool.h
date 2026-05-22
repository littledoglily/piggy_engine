#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// common/thread_pool.h  —  固定大小线程池
//
// 用法：
//   ThreadPool pool(4);
//   pool.submit([&]{ doWork(); });
//   pool.waitAll();   // 等待所有已提交任务完成
//   // pool 析构时自动 join 所有线程
// ─────────────────────────────────────────────────────────────────────────────
#include "common/blocking_queue.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ii {

class ThreadPool {
public:
    using Task = std::function<void()>;

    // capacity：内部任务队列容量（默认 4096，实际任务远少于此时不会阻塞 submit）
    explicit ThreadPool(size_t n_threads, size_t queue_capacity = 4096);
    ~ThreadPool();

    // 提交任务。队列满时阻塞调用方；队列关闭后返回（不 throw）。
    void submit(Task fn);

    // 阻塞直到所有已提交任务执行完毕。
    void waitAll();

    size_t threadCount() const { return threads_.size(); }

private:
    BlockingQueue<Task>      queue_;
    std::vector<std::thread> threads_;

    std::mutex               mu_;
    std::condition_variable  done_cv_;
    int                      active_tasks_ = 0;
};

} // namespace ii
