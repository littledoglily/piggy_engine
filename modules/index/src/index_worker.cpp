#include "index_worker.h"
#include <iostream>

namespace ii {

IndexWorker::IndexWorker(uint32_t worker_id,
                         const std::string& dir,
                         const Schema& schema,
                         const std::vector<std::unique_ptr<FieldDescriptor>>& descs,
                         std::atomic<uint32_t>& g_next_seg_id,
                         std::atomic<DocId>&    g_next_doc_id,
                         float ram_threshold_mb)
    : worker_id_(worker_id)
    , dir_(dir)
    , schema_(schema)
    , descs_(descs)
    , g_next_seg_id_(g_next_seg_id)
    , g_next_doc_id_(g_next_doc_id)
    , ram_threshold_mb_(ram_threshold_mb)
{}

// ─────────────────────────────────────────────────────────────────────────────
// run：消费队列直到关闭并排空
// ─────────────────────────────────────────────────────────────────────────────

void IndexWorker::run(BlockingQueue<DocRef>& queue) {
    DocRef doc_ref;
    while (queue.pop(doc_ref)) {
        processDoc(*doc_ref);
        maybeFlush();
    }
    // 队列关闭且已排空，flush 剩余数据
    doFlush();
}

// ─────────────────────────────────────────────────────────────────────────────
// processDoc：为文档分配全局 DocId，写入本地内存索引
// ─────────────────────────────────────────────────────────────────────────────

void IndexWorker::processDoc(Document& doc) {
    // 覆盖调用方的 doc_id，使用全局原子递增（1-indexed）
    doc.doc_id = g_next_doc_id_.fetch_add(1, std::memory_order_relaxed);
    ++local_doc_count_;

    uint32_t field_pos_base = 0;
    std::vector<Token> token_buf;

    for (const auto& desc : descs_) {
        token_buf.clear();
        field_pos_base = desc->buildTokensDoc(doc.doc_id, doc.fields,
                                               field_pos_base, analyzer_, token_buf);
        for (auto& tok : token_buf) {
            field_token_counts_[tok.field]++;
            field_indexes_[tok.field].addToken(tok);
        }

        auto it = doc.fields.find(desc->name());
        const FieldVal* val = (it != doc.fields.end()) ? &it->second : nullptr;
        desc->writeFast(val, ff_writer_);
    }

    // 组装 StoredDoc
    StoredDoc sd;
    sd.doc_id = doc.doc_id;
    sd.ext_id = doc.ext_id;
    for (const auto& desc : descs_) {
        if (!desc->isStored()) continue;
        auto it = doc.fields.find(desc->name());
        const FieldVal* val = (it != doc.fields.end()) ? &it->second : nullptr;
        std::string s = desc->storeStr(val);
        if (!s.empty()) sd.str_fields[desc->name()] = std::move(s);
    }
    stored_ram_bytes_ += sd.ramUsage();
    stored_docs_buf_.push_back(std::move(sd));

    // 同步各字段 InMemoryIndex 的 doc_count（用于 avgdl 计算）
    for (auto& [_, idx] : field_indexes_)
        idx.setDocCount(local_doc_count_);
}

// ─────────────────────────────────────────────────────────────────────────────
// maybeFlush / doFlush
// ─────────────────────────────────────────────────────────────────────────────

void IndexWorker::maybeFlush() {
    if (estimateRam() > static_cast<size_t>(ram_threshold_mb_ * 1024 * 1024))
        doFlush();
}

void IndexWorker::doFlush() {
    // 检查是否有数据
    bool any_data = false;
    for (const auto& [_, idx] : field_indexes_)
        if (idx.termCount() > 0) { any_data = true; break; }
    if (!any_data) return;

    // 取全局唯一 SegId（原子递增，不同 Worker 分配到不同值）
    uint32_t seg_id = g_next_seg_id_.fetch_add(1, std::memory_order_relaxed);

    // 计算各字段 avgdl
    std::map<std::string, float> field_avg_doc_lens;
    for (const auto& [field, cnt] : field_token_counts_) {
        field_avg_doc_lens[field] = local_doc_count_ > 0
            ? static_cast<float>(cnt) / static_cast<float>(local_doc_count_) : 0.f;
    }

    uint32_t n_docs = static_cast<uint32_t>(stored_docs_buf_.size());

    // 写倒排 + stored 文件
    SegmentWriter seg_writer(dir_, seg_id);
    auto seg_stats = seg_writer.flush(field_indexes_, stored_docs_buf_,
                                      n_docs, field_avg_doc_lens, schema_);

    // 写 FastField 文件
    ff_writer_.flush(dir_, seg_id);

    std::cout << "[Worker " << worker_id_ << "] Flushed seg=" << seg_id
              << " docs=" << n_docs
              << " terms=" << seg_stats.term_count << "\n";

    flushed_seg_ids_.push_back(seg_id);

    // 清空本地缓冲（local_doc_count_ 不清，累计总数）
    field_indexes_.clear();
    field_token_counts_.clear();
    stored_docs_buf_.clear();
    stored_ram_bytes_ = 0;
    ff_writer_.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// estimateRam
// ─────────────────────────────────────────────────────────────────────────────

size_t IndexWorker::estimateRam() const {
    size_t total = stored_ram_bytes_;
    for (const auto& [_, idx] : field_indexes_)
        total += idx.ramUsage();
    return total;
}

} // namespace ii
