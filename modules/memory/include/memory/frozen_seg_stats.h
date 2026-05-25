#pragma once
// memory/frozen_seg_stats.h — frozen segment 生命周期监控结构
//
// FrozenSegState 描述一个 frozen MemorySegment 从进入 FlushQueue 到
// 内存释放的全部状态流转。FrozenEntry 是 FlushQueue 中的传递单元。

#include "index/i_segment_reader.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

// forward declaration
namespace ii::memory { class MemorySegment; }

namespace ii {

enum class FrozenSegState {
    Queued,      // 进入 FlushQueue，等待 FlushWorker 取走
    Flushing,    // FlushWorker 正在调用 flushToDisk
    OnDisk,      // tmp segment 已写完，等待 merge（或直接作为 final）
    Merging,     // SegmentMerger 处理中
    Committed,   // disk reader 已进入 all_readers_，frozen reader 已移除
};

struct FrozenSegStats {
    uint32_t thread_id       = 0;
    uint32_t seg_id          = 0;
    uint32_t doc_count       = 0;
    uint64_t ram_bytes       = 0;
    float    hash_utilization = 0.f;
    FrozenSegState state     = FrozenSegState::Queued;

    using TimePoint = std::chrono::steady_clock::time_point;
    TimePoint freeze_time;
    TimePoint queue_time;
    TimePoint flush_start_time;
    TimePoint on_disk_time;
    TimePoint merge_start_time;
    TimePoint committed_time;
};

struct FrozenEntry {
    std::shared_ptr<ii::memory::MemorySegment> segment;       // 数据，由 FlushWorker 读取后写盘
    std::shared_ptr<ISegmentReader>            frozen_reader; // 待从 all_readers_ 移除
    std::shared_ptr<FrozenSegStats>            stats;
};

} // namespace ii
