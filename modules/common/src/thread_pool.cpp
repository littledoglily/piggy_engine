#include "common/thread_pool.h"

namespace ii {

ThreadPool::ThreadPool(size_t n_threads, size_t queue_capacity)
    : queue_(queue_capacity)
{
    threads_.reserve(n_threads);
    for (size_t i = 0; i < n_threads; ++i) {
        threads_.emplace_back([this] {
            Task fn;
            while (queue_.pop(fn)) {
                fn();
                // 任务完成，递减计数；若归零则通知 waitAll()
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    if (--active_tasks_ == 0) done_cv_.notify_all();
                }
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    queue_.close();
    for (auto& t : threads_) t.join();
}

void ThreadPool::submit(Task fn) {
    // 先递增计数再入队，保证 waitAll() 看到 active_tasks_ >= 1
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++active_tasks_;
    }
    if (!queue_.push(std::move(fn))) {
        // 队列已关闭（析构中），补偿递减
        std::lock_guard<std::mutex> lock(mu_);
        if (--active_tasks_ == 0) done_cv_.notify_all();
    }
}

void ThreadPool::waitAll() {
    std::unique_lock<std::mutex> lock(mu_);
    done_cv_.wait(lock, [&] { return active_tasks_ == 0; });
}

} // namespace ii
