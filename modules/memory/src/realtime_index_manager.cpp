#include "memory/realtime_index_manager.h"
#include <stdexcept>

namespace ii {

RealtimeIndexManager::RealtimeIndexManager(const std::string& dir,
                                           const Schema&       schema,
                                           IndexSearcher&      searcher,
                                           int                 n_rt_threads,
                                           FreezeConfig        freeze_cfg,
                                           uint32_t            global_seg_id_start)
    : dir_(dir)
    , schema_(schema)
    , searcher_(searcher)
    , n_rt_threads_(n_rt_threads)
    , freeze_cfg_(freeze_cfg)
    , global_seg_id_(global_seg_id_start)
{
    if (n_rt_threads_ <= 0)
        throw std::invalid_argument("n_rt_threads must be > 0");
}

RealtimeIndexManager::~RealtimeIndexManager() {
    stop();
}

void RealtimeIndexManager::start() {
    if (started_) return;
    started_ = true;

    // 创建 FlushWorker
    flush_worker_ = std::make_unique<FlushWorker>(
        dir_, schema_, flush_queue_, searcher_, global_seg_id_);
    flush_worker_->start();

    // 创建 RTIndexThread（构造时自动 addReader 到 IndexSearcher）
    rt_threads_.reserve(n_rt_threads_);
    for (int i = 0; i < n_rt_threads_; ++i) {
        rt_threads_.push_back(std::make_unique<RTIndexThread>(
            static_cast<uint32_t>(i),
            dir_, schema_, searcher_, flush_queue_, global_seg_id_, freeze_cfg_));
    }
}

void RealtimeIndexManager::addDocument(const Document& doc) {
    if (!started_ || stopped_)
        throw std::logic_error("RealtimeIndexManager: not running");

    // round-robin 分发
    uint64_t idx = round_robin_.fetch_add(1) % static_cast<uint64_t>(n_rt_threads_);
    rt_threads_[idx]->addDocument(doc);
}

void RealtimeIndexManager::stop() {
    if (!started_ || stopped_) return;
    stopped_ = true;

    // 1. 先 shutdown 所有 RTIndexThread（剩余 doc freeze 进 FlushQueue）
    for (auto& t : rt_threads_)
        t->shutdown();

    // 2. 再 stop FlushWorker（shutdown FlushQueue → 解除 wait，cleanup loop 排空剩余）
    if (flush_worker_)
        flush_worker_->stop();
}

} // namespace ii
