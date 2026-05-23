#include "index/parallel_index_writer.h"
#include "index_worker.h"
#include "common/file_utils.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <cmath>
#include <thread>

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// 并行 merge 辅助函数
// ─────────────────────────────────────────────────────────────────────────────

// 根据 temp segment 总数和 worker 数决定并行 merge 的分组数
static int computeGroupCount(size_t total, int n_workers) {
    if (total <= 8) return 1;
    int n = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(total))));
    return std::min(n, n_workers);
}

// 将 ids 均匀切分成 n_groups 组（轮询分配，每组大小差 ≤ 1）
static std::vector<std::vector<uint32_t>>
splitIntoGroups(const std::vector<uint32_t>& ids, int n_groups) {
    n_groups = std::min(n_groups, static_cast<int>(ids.size()));
    if (n_groups <= 1) return {ids};
    std::vector<std::vector<uint32_t>> groups(n_groups);
    for (size_t i = 0; i < ids.size(); ++i)
        groups[i % static_cast<size_t>(n_groups)].push_back(ids[i]);
    return groups;
}

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
        return;
    }

    if (temp_seg_ids.size() == 1) {
        // 只有 1 个 Segment，无需合并
        final_seg_ids_ = temp_seg_ids;
        std::cout << "[ParallelIndexWriter] Single segment, skipping merge.\n";
    } else {
        // 按 total 大小自动决策分组数，各组并行 merge
        int n_groups = computeGroupCount(temp_seg_ids.size(), n_workers_);
        auto groups  = splitIntoGroups(temp_seg_ids, n_groups);

        std::cout << "[ParallelIndexWriter] Merging " << temp_seg_ids.size()
                  << " temp segments → " << groups.size()
                  << " group(s) (parallel)...\n";

        // 预先分配所有输出 ID，避免 merge 线程内部竞争
        std::vector<uint32_t> out_ids;
        out_ids.reserve(groups.size());
        for (size_t i = 0; i < groups.size(); ++i)
            out_ids.push_back(g_next_seg_id_.fetch_add(1, std::memory_order_relaxed));

        // 各组并行 merge
        std::vector<std::thread> merge_threads;
        merge_threads.reserve(groups.size());
        for (size_t i = 0; i < groups.size(); ++i) {
            merge_threads.emplace_back([this, grp = groups[i], oid = out_ids[i]] {
                SegmentMerger merger(dir_, grp);
                merger.mergeAll(oid);
            });
        }
        for (auto& t : merge_threads) t.join();

        final_seg_ids_ = out_ids;
        std::sort(final_seg_ids_.begin(), final_seg_ids_.end());

        std::cout << "[ParallelIndexWriter] Merge complete → final segs=[";
        for (size_t i = 0; i < final_seg_ids_.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << final_seg_ids_[i];
        }
        std::cout << "]\n";
    }

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
