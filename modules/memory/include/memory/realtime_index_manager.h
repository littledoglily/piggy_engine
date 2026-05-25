#pragma once
// memory/realtime_index_manager.h — 实时索引管理器
//
// 组装 FlushQueue / FlushWorker / RTIndexThread[] 及对应 std::thread，
// 对外暴露 addDocument / start / stop 接口。
//
// 典型使用：
//   RealtimeIndexManager mgr(dir, schema, searcher, n_threads, freeze_cfg);
//   mgr.start();
//   mgr.addDocument(doc);   // round-robin 分发
//   mgr.stop();             // flush 全部剩余数据，等待 FlushWorker 完成

#include "memory/flush_queue.h"
#include "memory/flush_worker.h"
#include "memory/rt_index_thread.h"
#include "field/schema.h"
#include "core/types.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ii {

class IndexSearcher;

class RealtimeIndexManager {
public:
    // n_rt_threads：实时写入线程数（每个线程持有独立 MemorySegment）
    // freeze_cfg：触发 freeze 的阈值
    // global_seg_id_start：segment id 起始值，避免与已有 disk segment 冲突
    RealtimeIndexManager(const std::string&     dir,
                         const Schema&           schema,
                         IndexSearcher&          searcher,
                         int                     n_rt_threads,
                         FreezeConfig            freeze_cfg,
                         uint32_t                global_seg_id_start = 1000);

    ~RealtimeIndexManager();

    // 启动 FlushWorker 后台线程 + 注册所有 RTIndexThread 的 active reader
    void start();

    // round-robin 分发到某个 RTIndexThread
    void addDocument(const Document& doc);

    // 1. shutdown 所有 RTIndexThread（剩余 doc 进入 FlushQueue）
    // 2. 等待 FlushWorker 排空队列并完成最后一批 flush
    void stop();

    // 全局 segment id 计数器（供外部统一管理时使用）
    std::atomic<uint32_t>& globalSegId() { return global_seg_id_; }

private:
    std::string                                dir_;
    Schema                                     schema_;
    IndexSearcher&                             searcher_;
    int                                        n_rt_threads_;
    FreezeConfig                               freeze_cfg_;

    std::atomic<uint32_t>                      global_seg_id_;
    FlushQueue                                 flush_queue_;
    std::unique_ptr<FlushWorker>               flush_worker_;
    std::vector<std::unique_ptr<RTIndexThread>> rt_threads_;

    std::atomic<uint64_t>                      round_robin_{0};
    bool                                       started_ = false;
    bool                                       stopped_ = false;
};

} // namespace ii
