#pragma once
// memory/rt_index_thread.h — 实时写入线程上下文
//
// 每个实时写入线程持有独立的 MemorySegment（写入零锁）。
// addDocument 后检查 freeze 条件；触发时：
//   1. 新 MemorySegment + MemorySegmentReader → addReader（先加，保证可查）
//   2. 旧 frozen entry → FlushQueue
//   3. 切换 active 到新 segment
// shutdown() 强制 freeze 剩余文档，确保进入 FlushQueue。

#include "memory/flush_queue.h"
#include "memory/memory_segment.h"
#include "memory/memory_segment_reader.h"
#include "field/schema.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace ii {

class IndexSearcher;

struct FreezeConfig {
    size_t   ram_bytes_threshold;          // 主触发：e.g. 256 * 1024 * 1024
    float    hash_utilization_threshold;   // 次触发：e.g. 0.75f
};

class RTIndexThread {
public:
    RTIndexThread(uint32_t                thread_id,
                  const std::string&      dir,
                  const Schema&           schema,
                  IndexSearcher&          searcher,
                  FlushQueue&             queue,
                  std::atomic<uint32_t>&  global_seg_id,
                  FreezeConfig            cfg);

    ~RTIndexThread();

    void addDocument(const Document& doc);

    // 将剩余未 flush 的文档 freeze 进 FlushQueue，之后该线程不再写入
    void shutdown();

private:
    bool shouldFreeze() const;
    void freeze();                    // 原子 swap：先 addReader(new) → push(old)

    uint32_t               thread_id_;
    std::string            dir_;
    Schema                 schema_;
    IndexSearcher&         searcher_;
    FlushQueue&            queue_;
    std::atomic<uint32_t>& global_seg_id_;
    FreezeConfig           cfg_;

    std::shared_ptr<memory::MemorySegment> active_seg_;
    std::shared_ptr<ISegmentReader>        active_reader_;
    uint32_t                               active_seg_id_   = 0;
    DocId                                  next_local_id_   = 1; // segment 内部局部 DocId

    bool shut_down_ = false;
};

} // namespace ii
