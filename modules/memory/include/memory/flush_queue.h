#pragma once
// memory/flush_queue.h — 线程安全的 frozen segment 队列
//
// RTIndexThread 向队列 push FrozenEntry；
// FlushWorker 调用 takeBatch 取走一批（最多 max_batch 个），
// 超时后返回当前已有的（防止低流量时数据长期滞留内存）。

#include "memory/frozen_seg_stats.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace ii {

class FlushQueue {
public:
    void push(FrozenEntry entry);

    // 等待直到 queue.size() >= max_batch 或超过 timeout_ms，
    // 取走 min(size, max_batch) 个 entry。
    // shutdown() 后解除 wait，但队列有数据时仍继续返回（允许 drain）。
    std::vector<FrozenEntry> takeBatch(int max_batch, int timeout_ms);

    // 解除所有阻塞的 takeBatch 等待。shutdown 后队列仍可读，直到为空才返回空。
    void shutdown();

    size_t size() const;

private:
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::deque<FrozenEntry> queue_;
    bool                    shutdown_ = false;
};

} // namespace ii
