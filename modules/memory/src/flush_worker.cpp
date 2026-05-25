#include "memory/flush_worker.h"
#include "memory/memory_segment.h"
#include "index/segment_merger.h"
#include "index/segment_reader.h"
#include "query/index_searcher.h"
#include <chrono>
#include <filesystem>
#include <iostream>

namespace ii {

using memory::MemorySegment;
namespace fs = std::filesystem;

FlushWorker::FlushWorker(const std::string&     dir,
                         const Schema&          schema,
                         FlushQueue&            queue,
                         IndexSearcher&         searcher,
                         std::atomic<uint32_t>& global_seg_id)
    : dir_(dir)
    , schema_(schema)
    , queue_(queue)
    , searcher_(searcher)
    , global_seg_id_(global_seg_id)
{}

FlushWorker::~FlushWorker() {
    stop();
}

void FlushWorker::start() {
    thread_ = std::thread(&FlushWorker::run, this);
}

void FlushWorker::stop() {
    if (!stop_requested_.exchange(true)) {
        queue_.shutdown();
        if (thread_.joinable()) thread_.join();
    }
}

void FlushWorker::run() {
    while (!stop_requested_) {
        auto batch = queue_.takeBatch(3, 500);
        if (batch.empty()) continue;
        processBatch(batch);
    }
    // 退出前消费队列剩余数据
    while (true) {
        auto batch = queue_.takeBatch(3, 0);
        if (batch.empty()) break;
        processBatch(batch);
    }
}

void FlushWorker::processBatch(std::vector<FrozenEntry>& batch) {
    using Clock = std::chrono::steady_clock;

    // ── Phase 1：每个 frozen segment 独立写到 tmp disk segment ───────────────
    std::vector<uint32_t> tmp_ids;
    tmp_ids.reserve(batch.size());

    for (auto& entry : batch) {
        entry.stats->state           = FrozenSegState::Flushing;
        entry.stats->flush_start_time = Clock::now();

        uint32_t tmp_id = global_seg_id_.fetch_add(1);
        tmp_ids.push_back(tmp_id);

        entry.segment->flushToDisk(dir_, tmp_id);

        entry.stats->state        = FrozenSegState::OnDisk;
        entry.stats->on_disk_time = Clock::now();
    }

    // ── Phase 2：单个直接用，多个 k-way merge ─────────────────────────────────
    uint32_t final_id;
    if (batch.size() == 1) {
        final_id = tmp_ids[0];
    } else {
        for (auto& entry : batch) {
            entry.stats->state            = FrozenSegState::Merging;
            entry.stats->merge_start_time = Clock::now();
        }

        final_id = global_seg_id_.fetch_add(1);
        SegmentMerger merger(dir_, tmp_ids);
        merger.mergeAll(final_id);

        // 清理 tmp segments
        for (uint32_t tid : tmp_ids)
            fs::remove_all(dir_ + "/segment_" + std::to_string(tid));
    }

    // ── Phase 3：加载磁盘 reader，原子提交到 IndexSearcher ────────────────────
    auto disk_reader = std::make_shared<SegmentReader>(dir_, final_id);

    std::vector<std::shared_ptr<ISegmentReader>> frozen_readers;
    frozen_readers.reserve(batch.size());
    for (auto& entry : batch)
        frozen_readers.push_back(entry.frozen_reader);

    searcher_.commitDiskSegment(disk_reader, frozen_readers);

    // ── Phase 4：打点 Committed ───────────────────────────────────────────────
    auto now = Clock::now();
    for (auto& entry : batch) {
        entry.stats->state          = FrozenSegState::Committed;
        entry.stats->committed_time = now;
    }

    std::cout << "[FlushWorker] committed seg_" << final_id
              << " (" << batch.size() << " frozen merged, "
              << disk_reader->docCount() << " docs)\n";
}

} // namespace ii
