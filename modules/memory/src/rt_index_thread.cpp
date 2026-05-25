#include "memory/rt_index_thread.h"
#include "memory/frozen_seg_stats.h"
#include "memory/memory_segment_reader.h"
#include "query/index_searcher.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace ii {

using memory::MemorySegment;
using memory::MemorySegmentReader;

RTIndexThread::RTIndexThread(uint32_t                thread_id,
                             const std::string&      dir,
                             const Schema&           schema,
                             IndexSearcher&          searcher,
                             FlushQueue&             queue,
                             std::atomic<uint32_t>&  global_seg_id,
                             FreezeConfig            cfg)
    : thread_id_(thread_id)
    , dir_(dir)
    , schema_(schema)
    , searcher_(searcher)
    , queue_(queue)
    , global_seg_id_(global_seg_id)
    , cfg_(cfg)
{
    active_seg_id_ = global_seg_id_.fetch_add(1);
    active_seg_    = std::make_shared<MemorySegment>(schema_);
    active_reader_ = std::make_shared<MemorySegmentReader>(
        std::static_pointer_cast<const MemorySegment>(active_seg_));
    searcher_.addReader(active_reader_);
}

RTIndexThread::~RTIndexThread() {
    shutdown();
}

void RTIndexThread::addDocument(const Document& doc) {
    if (shut_down_) throw std::logic_error("RTIndexThread: addDocument after shutdown");
    active_seg_->addDocument(doc, next_local_id_++);
    if (shouldFreeze()) freeze();
}

void RTIndexThread::shutdown() {
    if (shut_down_) return;
    shut_down_ = true;
    if (active_seg_->docCount() > 0) freeze();
    // 空 segment（0 docs）：直接从 all_readers_ 移除，不进 FlushQueue
    else searcher_.removeReader(active_reader_);
}

bool RTIndexThread::shouldFreeze() const {
    return active_seg_->ramUsed()                      >= cfg_.ram_bytes_threshold ||
           active_seg_->hashtable().loadFactor()       >= cfg_.hash_utilization_threshold;
}

void RTIndexThread::freeze() {
    using Clock = std::chrono::steady_clock;

    // 构建 stats
    auto stats = std::make_shared<FrozenSegStats>();
    stats->thread_id        = thread_id_;
    stats->seg_id           = active_seg_id_;
    stats->doc_count        = active_seg_->docCount();
    stats->ram_bytes        = active_seg_->ramUsed();
    stats->hash_utilization = active_seg_->hashtable().loadFactor();
    stats->state            = FrozenSegState::Queued;
    stats->freeze_time      = Clock::now();

    // 1. 先建新 segment + reader，注册到 IndexSearcher（保证写入连续可查）
    uint32_t new_id    = global_seg_id_.fetch_add(1);
    auto new_seg       = std::make_shared<MemorySegment>(schema_);
    auto new_reader    = std::make_shared<MemorySegmentReader>(
        std::static_pointer_cast<const MemorySegment>(new_seg));
    searcher_.addReader(new_reader);

    // 2. 将旧 segment 打包进 FlushQueue（old reader 仍在 all_readers_，数据不丢）
    FrozenEntry entry;
    entry.segment       = active_seg_;
    entry.frozen_reader = active_reader_;
    entry.stats         = stats;

    stats->queue_time = Clock::now();
    queue_.push(std::move(entry));

    std::cout << "[RTIndexThread " << thread_id_ << "] freeze seg_" << active_seg_id_
              << " (" << stats->doc_count << " docs, "
              << stats->ram_bytes / 1024 << " KB)\n";

    // 3. 切换 active，局部 DocId 从 1 重新开始
    active_seg_    = new_seg;
    active_reader_ = new_reader;
    active_seg_id_ = new_id;
    next_local_id_ = 1;
}

} // namespace ii
