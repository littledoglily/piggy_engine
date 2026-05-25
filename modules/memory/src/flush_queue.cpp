#include "memory/flush_queue.h"

namespace ii {

void FlushQueue::push(FrozenEntry entry) {
    {
        std::lock_guard lock(mu_);
        queue_.push_back(std::move(entry));
    }
    cv_.notify_one();
}

std::vector<FrozenEntry> FlushQueue::takeBatch(int max_batch, int timeout_ms) {
    std::unique_lock lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        return shutdown_ || (int)queue_.size() >= max_batch;
    });

    if (queue_.empty()) return {};  // shutdown_ 只解除 wait，不拒绝读

    int take = std::min((int)queue_.size(), max_batch);
    std::vector<FrozenEntry> batch;
    batch.reserve(take);
    for (int i = 0; i < take; ++i) {
        batch.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return batch;
}

void FlushQueue::shutdown() {
    {
        std::lock_guard lock(mu_);
        shutdown_ = true;
    }
    cv_.notify_all();
}

size_t FlushQueue::size() const {
    std::lock_guard lock(mu_);
    return queue_.size();
}

} // namespace ii
