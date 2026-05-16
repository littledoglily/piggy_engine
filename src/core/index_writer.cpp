#include "core/index_writer.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace ii {

IndexWriter::IndexWriter(const std::string& dir, float ram_buffer_mb, Schema schema)
    : dir_(dir), ram_buffer_mb_(ram_buffer_mb), schema_(std::move(schema))
{
    std::filesystem::create_directories(dir_);
    schema_.save(dir_);   // 持久化 schema.json
    std::cout << "[IndexWriter] Initialized. Dir=" << dir_
              << " RamBuffer=" << ram_buffer_mb_ << "MB\n";
}

IndexWriter::~IndexWriter() {
    if (mem_index_.termCount() > 0) {
        flush();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 字段名 → Document 具名字段的桥接（向后兼容，Step 6 改为 map 后可删除）
// ─────────────────────────────────────────────────────────────────────────────

std::string IndexWriter::getDocText(const Document& doc, const std::string& name) {
    if (name == "title")    return doc.title;
    if (name == "body")     return doc.body;
    if (name == "category") return doc.category;
    if (name == "source")   return doc.source;
    return "";
}

int64_t IndexWriter::getDocInt64(const Document& doc, const std::string& name) {
    if (name == "pubtime") return doc.pubtime;
    if (name == "uid")     return doc.uid;
    return 0;
}

float IndexWriter::getDocFloat32(const Document& doc, const std::string& name) {
    if (name == "page_rank") return doc.page_rank;
    return 0.0f;
}

// ─────────────────────────────────────────────────────────────────────────────
// addDocument：按 Schema 路由各字段
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::addDocument(const Document& doc) {
    ++total_docs_;

    // ── 按 Schema 路由 ────────────────────────────────────────────────────────
    // 跨 Text 字段累积位置偏移，保证同一 doc 所有字段 token 位置单调递增
    // TODO: 后续不同field的索引要带字段名, 主要针对text类型索引，后续想要只在TEXT字段检索不支持
    uint32_t field_pos_base = 0;

    for (const auto& fs : schema_.fields) {

        if (fs.type == FieldType::Text || fs.type == FieldType::Keyword) {
            std::string val = getDocText(doc, fs.name);

            // 建倒排
            if (fs.index != IndexOption::None && !val.empty()) {
                if (fs.type == FieldType::Text) {
                    auto tokens = analyzer_.analyze(doc.doc_id, val);
                    total_tokens_ += tokens.size();
                    uint32_t max_pos = field_pos_base;
                    for (auto& tok : tokens) {
                        tok.position += field_pos_base;
                        if (tok.position > max_pos) max_pos = tok.position;
                        mem_index_.addToken(tok);
                    }
                    field_pos_base = max_pos + 1;
                } else {
                    // Keyword：整体作为一个 term
                    Token tok;
                    tok.term      = val;
                    tok.doc_id    = doc.doc_id;
                    tok.position  = field_pos_base++;
                    tok.start_off = 0;
                    tok.end_off   = static_cast<uint32_t>(val.size());
                    mem_index_.addToken(tok);
                }
            }

        } else if (fs.type == FieldType::Int64) {
            if (fs.fast) {
                ff_writer_.addInt64(fs.name, getDocInt64(doc, fs.name));
            }

        } else if (fs.type == FieldType::Float32) {
            if (fs.fast) {
                ff_writer_.addFloat32(fs.name, getDocFloat32(doc, fs.name));
            }
        }
    }

    // ── 原文存储（按 schema 中 stored=true 的字段组装 StoredDoc）────────────
    // StoredDoc 保持当前格式不变（Step 6 再泛化）
    StoredDoc sd;
    sd.doc_id   = doc.doc_id;
    sd.ext_id   = doc.ext_id;
    sd.source   = doc.source;
    sd.title    = doc.title;
    sd.body     = doc.body;
    sd.category = doc.category;
    stored_docs_buf_.push_back(std::move(sd));

    mem_index_.setDocCount(total_docs_);

    if (estimateRamUsage() > static_cast<size_t>(ram_buffer_mb_ * 1024 * 1024)) {
        std::cout << "[IndexWriter] RAM buffer full, auto-flushing...\n";
        flush();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// flush
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::flush() {
    if (mem_index_.termCount() == 0) return;

    float avg_doc_len = (total_docs_ > 0)
        ? static_cast<float>(total_tokens_) / static_cast<float>(total_docs_)
        : 0.0f;

    SegmentWriter seg_writer(dir_, next_seg_id_);
    seg_writer.flush(mem_index_, stored_docs_buf_,
                     static_cast<uint32_t>(stored_docs_buf_.size()),
                     avg_doc_len);

    // FastField 独立 flush（IndexWriter 直接负责）
    ff_writer_.flush(dir_, next_seg_id_);

    ++next_seg_id_;
    mem_index_ = InMemoryIndex();
    stored_docs_buf_.clear();
    ff_writer_.clear();
    total_tokens_ = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// commit
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::commit() {
    flush();

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
// estimateRamUsage
// ─────────────────────────────────────────────────────────────────────────────

size_t IndexWriter::estimateRamUsage() const {
    size_t estimate = 0;
    auto terms = mem_index_.sortedTerms();
    for (const auto& t : terms) {
        const PostingList* pl = mem_index_.getPostingList(t);
        if (pl) estimate += t.size() + pl->size() * 32;
    }
    for (const auto& sd : stored_docs_buf_) {
        estimate += sd.title.size() + sd.body.size() + sd.category.size() + 16;
    }
    return estimate;
}

// ─────────────────────────────────────────────────────────────────────────────
// deleteDocument（软删除，简化实现）
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::deleteDocument(DocId doc_id) {
    std::cout << "[IndexWriter] deleteDocument(" << doc_id << ") - "
              << "marking as deleted (simplified)\n";
}

} // namespace ii
