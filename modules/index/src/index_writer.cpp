#include "index/index_writer.h"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <cstdio>

namespace ii {

IndexWriter::IndexWriter(const std::string& dir, float ram_buffer_mb, Schema schema)
    : dir_(dir), ram_buffer_mb_(ram_buffer_mb), schema_(std::move(schema))
    , descriptors_(buildDescriptors(schema_))
{
    std::filesystem::create_directories(dir_);
    schema_.save(dir_);
    std::cout << "[IndexWriter] Initialized. Dir=" << dir_
              << " RamBuffer=" << ram_buffer_mb_ << "MB\n";
}

IndexWriter::~IndexWriter() {
    for (const auto& [_, idx] : field_indexes_)
        if (idx.termCount() > 0) { flush(); break; }
}

// ─────────────────────────────────────────────────────────────────────────────
// addDocument：按 Schema 路由各字段
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::addDocument(const Document& doc) {
    ++total_docs_;

    // 跨字段累积位置偏移，保证同一 doc 所有字段 token 位置单调递增
    uint32_t field_pos_base = 0;
    std::vector<Token> token_buf;

    for (const auto& desc : descriptors_) {
        // 建倒排：文档级入口（CombinedField 联合多源，普通字段按名查找单值）
        token_buf.clear();
        field_pos_base = desc->buildTokensDoc(doc.doc_id, doc.fields,
                                               field_pos_base, analyzer_, token_buf);
        for (auto& tok : token_buf) {
            field_token_counts_[tok.field]++;
            field_indexes_[tok.field].addToken(tok);
        }

        // FastField 写入（定长字段始终写以保持数组对齐，缺失时写默认 0）
        auto it = doc.fields.find(desc->name());
        const FieldVal* val = (it != doc.fields.end()) ? &it->second : nullptr;
        desc->writeFast(val, ff_writer_);
    }

    // ── StoredDoc 组装（按描述符泛化）──────────────────────────────────────
    StoredDoc sd;
    sd.doc_id = doc.doc_id;
    sd.ext_id = doc.ext_id;
    for (const auto& desc : descriptors_) {
        if (!desc->isStored()) continue;
        auto it = doc.fields.find(desc->name());
        const FieldVal* val = (it != doc.fields.end()) ? &it->second : nullptr;
        std::string s = desc->storeStr(val);
        if (!s.empty()) sd.str_fields[desc->name()] = std::move(s);
    }
    stored_ram_bytes_ += sd.ramUsage();
    stored_docs_buf_.push_back(std::move(sd));

    for (auto& [_, idx] : field_indexes_) idx.setDocCount(total_docs_);

    if (estimateRamUsage() > static_cast<size_t>(ram_buffer_mb_ * 1024 * 1024)) {
        std::cout << "[IndexWriter] RAM buffer full, auto-flushing...\n";
        flush();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// flush
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::flush() {
    bool any_data = false;
    for (const auto& [_, idx] : field_indexes_)
        if (idx.termCount() > 0) { any_data = true; break; }
    if (!any_data) return;

    // per-field avg_doc_len
    std::map<std::string, float> field_avg_doc_lens;
    for (const auto& [field, cnt] : field_token_counts_) {
        field_avg_doc_lens[field] = total_docs_ > 0
            ? static_cast<float>(cnt) / static_cast<float>(total_docs_) : 0.f;
    }

    uint32_t n_docs = static_cast<uint32_t>(stored_docs_buf_.size());
    SegmentWriter seg_writer(dir_, next_seg_id_);
    auto seg_stats = seg_writer.flush(field_indexes_, stored_docs_buf_,
                                      n_docs, field_avg_doc_lens, schema_);

    auto ff_stats = ff_writer_.flush(dir_, next_seg_id_);

    printFlushStats(seg_stats, ff_stats);

    seg_stats_history_.push_back(seg_stats);
    ff_stats_history_.push_back(ff_stats);

    ++next_seg_id_;
    field_indexes_.clear();
    field_token_counts_.clear();
    stored_docs_buf_.clear();
    stored_ram_bytes_ = 0;
    ff_writer_.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// printFlushStats
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::printFlushStats(const SegmentWriteStats& seg,
                                  const FFWriteStats&      ff)
{
    auto kb = [](uint64_t b) { return static_cast<double>(b) / 1024.0; };
    auto ms = [](uint64_t us) { return static_cast<double>(us) / 1000.0; };

    uint64_t total_bytes = seg.totalSegBytes() + ff.total_bytes;
    uint64_t total_us    = seg.totalSegUs()    + ff.total_us;

    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║  Segment _%u — Build Stats%*s║\n",
           seg.segment_id, (int)(36 - std::to_string(seg.segment_id).size()), "");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║  Docs: %5u   Terms: %5u%*s║\n",
           seg.doc_count, seg.term_count, 30, "");
    printf("╠═══════════════ Posting List ═════════════════════════════════╣\n");
    printf("║  Total entries : %7llu  (avg %6.1f / term)%*s║\n",
           (unsigned long long)seg.total_pl_entries, seg.avg_pl_df, 12, "");
    printf("║  Max df        : %7u  term=\"%s\"%*s║\n",
           seg.max_pl_df, seg.max_pl_term.c_str(),
           (int)(20 - (int)seg.max_pl_term.size()), "");
    printf("║  Skip nodes    : %7llu  total  (avg %5.2f / term)%*s║\n",
           (unsigned long long)seg.total_skip_nodes,
           seg.avg_skip_nodes_per_term, 9, "");
    printf("║  Max skip nodes: %7u  term=\"%s\"%*s║\n",
           seg.max_skip_nodes, seg.max_skip_term.c_str(),
           (int)(20 - (int)seg.max_skip_term.size()), "");
    printf("╠═══════════════  File Sizes  ═════════════════════════════════╣\n");
    printf("║  .tim        : %8.1f KB  [%6.2f ms]%*s║\n",
           kb(seg.tim_bytes), ms(seg.tim_us), 18, "");
    printf("║  .doc        : %8.1f KB  [%6.2f ms]%*s║\n",
           kb(seg.doc_bytes), ms(seg.doc_us), 18, "");
    printf("║  .pos        : %8.1f KB  [%6.2f ms]%*s║\n",
           kb(seg.pos_bytes), ms(seg.pos_us), 18, "");
    printf("║  .fdt + .fdx : %8.1f KB  [%6.2f ms]%*s║\n",
           kb(seg.fdt_bytes + seg.fdx_bytes), ms(seg.fdt_fdx_us), 18, "");
    printf("║  .liv        : %8.1f KB%*s║\n", kb(seg.liv_bytes), 27, "");
    printf("║  .si         : %8.1f KB%*s║\n", kb(seg.si_bytes),  27, "");
    if (!ff.file_bytes.empty()) {
        bool first = true;
        for (const auto& [field, bytes] : ff.file_bytes) {
            if (first) {
                printf("║  .ff_%-9s: %8.1f KB  } ff total: %5.2f ms%*s║\n",
                       field.c_str(), kb(bytes), ms(ff.total_us), 6, "");
                first = false;
            } else {
                printf("║  .ff_%-9s: %8.1f KB%*s║\n",
                       field.c_str(), kb(bytes), 27, "");
            }
        }
    }
    printf("╠═══════════════  Totals  ═════════════════════════════════════╣\n");
    printf("║  Total size  : %8.1f KB%*s║\n", kb(total_bytes), 27, "");
    printf("║  Total time  : %8.2f ms%*s║\n", ms(total_us),    27, "");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
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
    size_t total = stored_ram_bytes_;
    for (const auto& [_, idx] : field_indexes_)
        total += idx.ramUsage();
    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
// deleteDocument（软删除，简化实现）
// ─────────────────────────────────────────────────────────────────────────────

void IndexWriter::deleteDocument(DocId doc_id) {
    std::cout << "[IndexWriter] deleteDocument(" << doc_id << ") - "
              << "marking as deleted (simplified)\n";
}

} // namespace ii
