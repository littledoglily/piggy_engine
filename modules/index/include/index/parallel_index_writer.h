#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index/parallel_index_writer.h  —  多线程文档写入入口
//
// 使用方式与 IndexWriter 基本一致，差异：
//   - addDocument 接受 Document&&（移动语义，避免字符串深拷贝）
//   - doc_id 由内部全局原子计数器分配，调用方无需设置
//   - commit() 会等待所有 Worker 完成，并写出 segments_N 文件
//
// Step 2 版本：Worker 各自独立产出 Segment，commit() 直接将所有 Segment
//             写入 segments 文件，不做 K-way 合并（Step 5 引入合并）
// ─────────────────────────────────────────────────────────────────────────────
#include "common/blocking_queue.h"
#include "common/doc_ref.h"
#include "field/schema.h"
#include "field/field_descriptor.h"
#include "index/segment_writer.h"
#include "index/segment_merger.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace ii {

// 前置声明（内部类，避免把 index_worker.h 暴露给外部）
class IndexWorker;

class ParallelIndexWriter {
public:
    // n_workers          ：消费线程数（推荐 = CPU 核数）
    // ram_per_worker_mb  ：每个 Worker 的内存阈值，超出则自动 flush
    // queue_capacity     ：生产者-消费者队列容量，满时阻塞 addDocument()
    ParallelIndexWriter(const std::string& dir,
                        int    n_workers         = 4,
                        float  ram_per_worker_mb = 64.0f,
                        Schema schema             = Schema::defaultSchema(),
                        size_t queue_capacity     = 1024);
    ~ParallelIndexWriter();

    // 移动语义入队，doc_id 由内部分配（调用方设置的 doc_id 会被覆盖）
    // 队列满时阻塞调用方（背压）
    void addDocument(Document&& doc);

    // 关闭队列 → 等待所有 Worker 结束 → 写 segments_N 文件
    // 关闭队列 → 等待所有 Worker 结束 → K-way 合并所有临时 Segment → 写 segments 文件
    void commit();

    uint32_t totalDocs()    const { return g_next_doc_id_.load() - 1; }
    uint32_t segmentCount() const { return final_seg_ids_.size(); }

    // 最终 Segment ID 列表（commit() 后有效；合并后通常只有 1 个）
    const std::vector<uint32_t>& segmentIds() const { return final_seg_ids_; }

    // commit() 前可查询各 Worker 产出的临时 Segment 数量（调试用）
    size_t workerTempSegCount() const;
    // 返回所有 Worker 的临时 seg_ids（commit() 前调用才有意义）
    std::vector<uint32_t> allWorkerSegIds() const;

private:
    void startWorkers();
    void stopAndJoin();
    void writeSegmentsFile();

    // ── 配置 ─────────────────────────────────────────────────────────────────
    std::string dir_;
    int         n_workers_;
    float       ram_per_worker_mb_;
    Schema      schema_;
    std::vector<std::unique_ptr<FieldDescriptor>> descs_;

    // ── 全局计数器（两个 Worker 的 fetch_add 天然隔离）────────────────────────
    std::atomic<uint32_t> g_next_seg_id_{0};
    std::atomic<DocId>    g_next_doc_id_{1};   // DocId 1-indexed

    // ── 并发基础设施 ─────────────────────────────────────────────────────────
    std::unique_ptr<BlockingQueue<DocRef>> queue_;
    std::vector<std::unique_ptr<IndexWorker>> workers_;
    std::vector<std::thread>                  threads_;

    // ── commit() 后填充 ───────────────────────────────────────────────────────
    std::vector<uint32_t> final_seg_ids_;
    bool committed_ = false;
};

} // namespace ii
