#include "index/parallel_index_writer.h"
#include "index_worker.h"
#include "common/file_utils.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// 构造
// ─────────────────────────────────────────────────────────────────────────────

ParallelIndexWriter::ParallelIndexWriter(const std::string& dir,
                                         int    n_workers,
                                         float  ram_per_worker_mb,
                                         Schema schema,
                                         size_t queue_capacity)
    : dir_(dir)
    , n_workers_(n_workers)
    , ram_per_worker_mb_(ram_per_worker_mb)
    , schema_(std::move(schema))
    , descs_(buildDescriptors(schema_))
    , queue_(std::make_unique<BlockingQueue<DocRef>>(queue_capacity))
{
    file_utils::ensureDir(dir_);
    schema_.save(dir_);

    std::cout << "[ParallelIndexWriter] dir=" << dir_
              << " workers=" << n_workers_
              << " ram_per_worker=" << ram_per_worker_mb_ << "MB\n";

    startWorkers();
}

ParallelIndexWriter::~ParallelIndexWriter() {
    if (!committed_) {
        // 未显式 commit，自动 commit（保证数据落盘）
        try { commit(); } catch (...) {}
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// startWorkers：创建 Worker 实例并启动线程
// ─────────────────────────────────────────────────────────────────────────────

void ParallelIndexWriter::startWorkers() {
    workers_.reserve(n_workers_);
    threads_.reserve(n_workers_);

    for (int i = 0; i < n_workers_; ++i) {
        workers_.push_back(std::make_unique<IndexWorker>(
            static_cast<uint32_t>(i),
            dir_,
            schema_,
            descs_,
            g_next_seg_id_,
            g_next_doc_id_,
            ram_per_worker_mb_
        ));

        IndexWorker* worker = workers_.back().get();
        threads_.emplace_back([this, worker] {
            worker->run(*queue_);
        });
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// addDocument：入队（移动语义，队列满时阻塞）
// ─────────────────────────────────────────────────────────────────────────────

void ParallelIndexWriter::addDocument(Document&& doc) {
    if (committed_)
        throw std::runtime_error("ParallelIndexWriter::addDocument called after commit()");
    queue_->push(makeDocRef(std::move(doc)));
}

// ─────────────────────────────────────────────────────────────────────────────
// commit：关闭队列 → 等待 Worker → K-way 合并临时 Segment → 写 segments 文件
// ─────────────────────────────────────────────────────────────────────────────

void ParallelIndexWriter::commit() {
    if (committed_) return;
    committed_ = true;

    // 1. 关闭队列：阻塞的 pop() 排干剩余元素后返回 false，Worker 自然退出
    queue_->close();

    // 2. join 所有 Worker 线程
    stopAndJoin();

    // 3. 汇总所有 Worker 产出的临时 Segment ID
    std::vector<uint32_t> temp_seg_ids;
    uint32_t worker_total_docs = 0;
    for (const auto& w : workers_) {
        for (uint32_t id : w->flushedSegIds())
            temp_seg_ids.push_back(id);
        worker_total_docs += w->docCount();
    }
    std::sort(temp_seg_ids.begin(), temp_seg_ids.end());

    std::cout << "[ParallelIndexWriter] Workers done."
              << " total_docs=" << worker_total_docs
              << " temp_segments=" << temp_seg_ids.size() << " ids=[";
    for (size_t i = 0; i < temp_seg_ids.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << temp_seg_ids[i];
    }
    std::cout << "]\n";

    // 4. K-way 合并
    if (temp_seg_ids.empty()) {
        // 没有任何文档，不产生 Segment
        std::cout << "[ParallelIndexWriter] No data, skipping merge.\n";
        writeSegmentsFile();
        return;
    }

    if (temp_seg_ids.size() == 1) {
        // 只有 1 个 Segment，无需合并
        final_seg_ids_ = temp_seg_ids;
        std::cout << "[ParallelIndexWriter] Single segment, skipping merge.\n";
    } else {
        // 多个 Segment：K-way 合并 → 1 个最终 Segment
        uint32_t final_id = g_next_seg_id_.fetch_add(1, std::memory_order_relaxed);
        std::cout << "[ParallelIndexWriter] Merging " << temp_seg_ids.size()
                  << " segments → seg=" << final_id << " ...\n";

        SegmentMerger merger(dir_, temp_seg_ids);
        merger.mergeAll(final_id);           // 合并、清理临时文件、更新 readers_
        final_seg_ids_ = {final_id};

        std::cout << "[ParallelIndexWriter] Merge complete → seg=" << final_id << "\n";
    }

    // 5. 写 segments_N 文件（覆盖 Merger 写的版本，保持格式统一）
    writeSegmentsFile();
}

// ─────────────────────────────────────────────────────────────────────────────
// stopAndJoin
// ─────────────────────────────────────────────────────────────────────────────

void ParallelIndexWriter::stopAndJoin() {
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writeSegmentsFile
// ─────────────────────────────────────────────────────────────────────────────

void ParallelIndexWriter::writeSegmentsFile() {
    uint32_t gen = static_cast<uint32_t>(final_seg_ids_.size());
    std::string path = dir_ + "/segments_" + std::to_string(gen);
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write segments file: " + path);

    f << "segment_count=" << gen << "\n";
    for (uint32_t id : final_seg_ids_)
        f << "segment_" << id << "\n";

    std::cout << "[ParallelIndexWriter] Wrote " << path << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// 调试辅助：查询所有 Worker 临时产出的 seg_ids
// ─────────────────────────────────────────────────────────────────────────────

size_t ParallelIndexWriter::workerTempSegCount() const {
    size_t n = 0;
    for (const auto& w : workers_) n += w->flushedSegIds().size();
    return n;
}

std::vector<uint32_t> ParallelIndexWriter::allWorkerSegIds() const {
    std::vector<uint32_t> ids;
    for (const auto& w : workers_)
        for (uint32_t id : w->flushedSegIds())
            ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace ii
