#include "core/index_writer.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace ii {

IndexWriter::IndexWriter(const std::string& dir, float ram_buffer_mb)
    : dir_(dir), ram_buffer_mb_(ram_buffer_mb)
{
    // 创建索引目录
    std::filesystem::create_directories(dir_);
    std::cout << "[IndexWriter] Initialized. Dir=" << dir_
              << " RamBuffer=" << ram_buffer_mb_ << "MB\n";
}

IndexWriter::~IndexWriter() {
    // 析构时自动 flush 剩余数据
    if (mem_index_.termCount() > 0) {
        flush();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// addDocument：写入一篇文档
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::addDocument(const Document& doc) {
    ++total_docs_;

    // 1. 文本分析（title + body 合并分析）
    std::string full_text = doc.title + " " + doc.body;
    std::vector<Token> tokens = analyzer_.analyze(doc.doc_id, full_text);
    total_tokens_ += tokens.size();

    // 2. 写入内存倒排索引
    for (const auto& tok : tokens) {
        mem_index_.addToken(tok);
    }

    // 3. 暂存原文
    StoredDoc sd;
    sd.doc_id   = doc.doc_id;
    sd.title    = doc.title;
    sd.body     = doc.body;
    sd.category = doc.category;
    stored_docs_buf_.push_back(std::move(sd));

    // 4. 暂存数值列（与 stored_docs_buf_ 并行，下标一一对应）
    FastFieldDoc ffd;
    ffd.pubtime   = doc.pubtime;
    ffd.uid       = doc.uid;
    ffd.page_rank = doc.page_rank;
    ff_buf_.push_back(ffd);

    mem_index_.setDocCount(total_docs_);

    // 4. 检查是否需要 flush
    if (estimateRamUsage() > (size_t)(ram_buffer_mb_ * 1024 * 1024)) {
        std::cout << "[IndexWriter] RAM buffer full, auto-flushing...\n";
        flush();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// flush：将内存索引写入磁盘 Segment
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::flush() {
    if (mem_index_.termCount() == 0) return;

    float avg_doc_len = (total_docs_ > 0)
        ? (float)total_tokens_ / (float)total_docs_
        : 0.0f;

    SegmentWriter writer(dir_, next_seg_id_);
    writer.flush(mem_index_, stored_docs_buf_, ff_buf_,
                 static_cast<uint32_t>(stored_docs_buf_.size()),
                 avg_doc_len);

    ++next_seg_id_;
    mem_index_ = InMemoryIndex();
    stored_docs_buf_.clear();
    ff_buf_.clear();
    total_tokens_ = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// commit：flush + 写 segments_N 注册表
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::commit() {
    flush();

    // 写 segments_N 文件（记录所有有效 Segment ID）
    std::string seg_file = dir_ + "/segments_" + std::to_string(next_seg_id_);
    std::ofstream f(seg_file, std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write segments_N");

    f << "segment_count=" << next_seg_id_ << "\n";
    for (uint32_t i = 0; i < next_seg_id_; ++i) {
        f << "segment_" << i << "\n";
    }
    std::cout << "[IndexWriter] Committed. Segments: " << next_seg_id_ << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// estimateRamUsage：粗估内存用量（字节）
// ─────────────────────────────────────────────────────────────────────────────

size_t IndexWriter::estimateRamUsage() const {
    // 粗估：每个 PostingEntry 约 32 字节 + term 字符串
    size_t estimate = 0;
    auto terms = mem_index_.sortedTerms();
    for (const auto& t : terms) {
        const PostingList* pl = mem_index_.getPostingList(t);
        if (pl) {
            estimate += t.size() + pl->size() * 32;
        }
    }
    // 加上原文缓冲
    for (const auto& sd : stored_docs_buf_) {
        estimate += sd.title.size() + sd.body.size() + sd.category.size() + 16;
    }
    return estimate;
}

// ─────────────────────────────────────────────────────────────────────────────
// deleteDocument：软删除（需要 SegmentReader 才能修改 .liv）
// 此处简化：记录到待删除列表，commit 时处理
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::deleteDocument(DocId doc_id) {
    // 在真实实现中，这里应找到包含该 doc_id 的 Segment 并修改其 .liv
    // 简化：打印提示
    std::cout << "[IndexWriter] deleteDocument(" << doc_id << ") - "
              << "marking as deleted (simplified)\n";
}

} // namespace ii
