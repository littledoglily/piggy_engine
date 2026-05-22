#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// common/blocking_queue.h  —  有界阻塞队列（MPMC）
//
// 语义：
//   push()  — 队列满时阻塞生产者；队列关闭后返回 false
//   pop()   — 队列空时阻塞消费者；关闭且空后返回 false（先排干残余元素）
//   close() — 广播关闭信号，生产者 push 返回 false，消费者排干后 pop 返回 false
// ─────────────────────────────────────────────────────────────────────────────
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace ii {

template<typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t capacity) : cap_(capacity) {}

    // 禁止拷贝和移动（内部持有 mutex）
    BlockingQueue(const BlockingQueue&)            = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    // 将 item 入队。队列满时阻塞，直到有空位或队列关闭。
    // 返回 false 表示队列已关闭，item 未入队。
    bool push(T item) {
        std::unique_lock<std::mutex> lock(mu_);
        not_full_.wait(lock, [&] { return buf_.size() < cap_ || closed_; });
        if (closed_) return false;
        buf_.push_back(std::move(item));
        not_empty_.notify_one();
        return true;
    }

    // 从队列取出一个元素写入 out。队列空时阻塞。
    // 返回 false 表示队列已关闭且已排空，没有更多元素。
    bool pop(T& out) {
        std::unique_lock<std::mutex> lock(mu_);
        not_empty_.wait(lock, [&] { return !buf_.empty() || closed_; });
        if (buf_.empty()) return false;   // closed + empty
        out = std::move(buf_.front());
        buf_.pop_front();
        not_full_.notify_one();
        return true;
    }

    // 关闭队列。唤醒所有阻塞的 push/pop，后续 push 返回 false，
    // pop 排干剩余元素后返回 false。
    void close() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    bool   closed() const { std::lock_guard<std::mutex> l(mu_); return closed_; }
    size_t size()   const { std::lock_guard<std::mutex> l(mu_); return buf_.size(); }
    bool   empty()  const { std::lock_guard<std::mutex> l(mu_); return buf_.empty(); }
    size_t capacity() const { return cap_; }

private:
    const size_t            cap_;
    std::deque<T>           buf_;
    bool                    closed_ = false;
    mutable std::mutex      mu_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
};

} // namespace ii
