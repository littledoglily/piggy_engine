#pragma once
// memory/flush_worker.h — 异步 flush 工作线程
//
// 从 FlushQueue 取 batch（最多 3 个）→ flushToDisk → k-way merge → commitDiskSegment。
// 全程不阻塞 RTIndexThread 的写入路径。

#include "memory/flush_queue.h"
#include "field/schema.h"
#include <atomic>
#include <string>
#include <thread>

namespace ii {

class IndexSearcher;

class FlushWorker {
public:
    FlushWorker(const std::string&     dir,
                const Schema&          schema,
                FlushQueue&            queue,
                IndexSearcher&         searcher,
                std::atomic<uint32_t>& global_seg_id);

    ~FlushWorker();

    void start();
    // 通知 FlushQueue shutdown，等待后台线程处理完当前 batch 后退出
    void stop();

private:
    void run();
    void processBatch(std::vector<FrozenEntry>& batch);

    std::string            dir_;
    Schema                 schema_;
    FlushQueue&            queue_;
    IndexSearcher&         searcher_;
    std::atomic<uint32_t>& global_seg_id_;

    std::thread            thread_;
    std::atomic<bool>      stop_requested_{false};
};

} // namespace ii
