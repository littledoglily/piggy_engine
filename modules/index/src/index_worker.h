#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index/src/index_worker.h  —  模块内部头文件，不对外暴露
//
// 每个 IndexWorker 对应一个独立的消费线程：
//   - 从 BlockingQueue 取文档，独立管理内存（无锁访问）
//   - 内存达阈值时用全局唯一 SegId 独立 flush，文件名天然无竞争
//   - queue 关闭后排干队列，flush 剩余数据，run() 返回
// ─────────────────────────────────────────────────────────────────────────────
#include "common/blocking_queue.h"
#include "common/doc_ref.h"
#include "analysis/analyzer.h"
#include "field/field_descriptor.h"
#include "field/fast_field_writer.h"
#include "field/schema.h"
#include "store/posting_list.h"
#include "index/segment_writer.h"
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ii {

class IndexWorker {
public:
    IndexWorker(uint32_t worker_id,
                const std::string& dir,
                const Schema& schema,
                const std::vector<std::unique_ptr<FieldDescriptor>>& descs,
                std::atomic<uint32_t>& g_next_seg_id,
                std::atomic<DocId>&    g_next_doc_id,
                float ram_threshold_mb);

    // 禁止拷贝/移动（持有原子量引用和 mutex）
    IndexWorker(const IndexWorker&)            = delete;
    IndexWorker& operator=(const IndexWorker&) = delete;

    // 持续消费 queue，关闭且排空后 flush 剩余数据，返回
    void run(BlockingQueue<DocRef>& queue);

    // run() 返回后可安全读取
    const std::vector<uint32_t>& flushedSegIds() const { return flushed_seg_ids_; }
    uint32_t docCount() const { return local_doc_count_; }

private:
    // 取出 doc，分配全局 DocId，写入本地内存索引
    void processDoc(Document& doc);
    // 判断内存是否超阈值，超时调用 doFlush()
    void maybeFlush();
    // 将本地内存索引写成 Segment 文件，清空内存缓冲
    void doFlush();
    // 估算当前内存占用（字节）
    size_t estimateRam() const;

    // ── 标识 ──────────────────────────────────────────────────────────────────
    uint32_t    worker_id_;
    std::string dir_;

    // ── Schema / 字段描述符（只读，跨线程共享，无需锁）──────────────────────
    const Schema&                                        schema_;
    const std::vector<std::unique_ptr<FieldDescriptor>>& descs_;
    Analyzer                                             analyzer_;  // 每 Worker 独立实例

    // ── 全局计数器（原子量引用，来自 ParallelIndexWriter）────────────────────
    std::atomic<uint32_t>& g_next_seg_id_;
    std::atomic<DocId>&    g_next_doc_id_;
    float                  ram_threshold_mb_;

    // ── 本地内存缓冲（只有本 Worker 线程访问，无锁）────────────────────────
    std::map<std::string, InMemoryIndex> field_indexes_;
    std::map<std::string, uint64_t>      field_token_counts_;
    std::vector<StoredDoc>               stored_docs_buf_;
    FastFieldWriter                      ff_writer_;
    size_t                               stored_ram_bytes_ = 0;
    uint32_t                             local_doc_count_  = 0;

    // ── 产出 ─────────────────────────────────────────────────────────────────
    std::vector<uint32_t> flushed_seg_ids_;
};

} // namespace ii
