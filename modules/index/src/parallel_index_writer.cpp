#include "index/parallel_index_writer.h"
#include "index_worker.h"
#include "common/file_utils.h"
#include <filesystem>
#include <fstream>
#include <iostream>
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
// commit：关闭队列 → 等待 Worker → 汇总 SegId → 写 segments 文件
// ─────────────────────────────────────────────────────────────────────────────

void ParallelIndexWriter::commit() {
    if (committed_) return;
    committed_ = true;

    // 1. 关闭队列：阻塞的 pop() 排干剩余元素后返回 false，Worker 自然退出
    queue_->close();

    // 2. join 所有 Worker 线程
    stopAndJoin();

    // 3. 汇总所有 Worker 产出的 Segment ID
    for (const auto& w : workers_) {
        for (uint32_t id : w->flushedSegIds())
            final_seg_ids_.push_back(id);
    }
    std::sort(final_seg_ids_.begin(), final_seg_ids_.end());

    uint32_t total = totalDocs();
    std::cout << "[ParallelIndexWriter] Committed. total_docs=" << total
              << " segments=" << final_seg_ids_.size() << " ids=[";
    for (size_t i = 0; i < final_seg_ids_.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << final_seg_ids_[i];
    }
    std::cout << "]\n";

    // 4. 写 segments_N 文件（N = 产出 Segment 总数，与 IndexWriter 保持一致）
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

} // namespace ii
