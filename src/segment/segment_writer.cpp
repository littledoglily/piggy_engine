#include "segment/segment_writer.h"
#include "postings/pfor_delta.h"
#include "postings/skiplist.h"
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <cmath>
#include <ctime>
#include <stdexcept>
#include <iostream>

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：将各种类型写入 ofstream
// ─────────────────────────────────────────────────────────────────────────────
static void writeU32(std::ofstream& f, uint32_t v) {
    f.write(reinterpret_cast<const char*>(&v), 4);
}
static void writeU64(std::ofstream& f, uint64_t v) {
    f.write(reinterpret_cast<const char*>(&v), 8);
}
static void writeF32(std::ofstream& f, float v) {
    f.write(reinterpret_cast<const char*>(&v), 4);
}
static void writeStr(std::ofstream& f, const std::string& s) {
    uint32_t len = static_cast<uint32_t>(s.size());
    writeU32(f, len);
    f.write(s.data(), len);
}
static void writeBytes(std::ofstream& f, const std::vector<uint8_t>& b) {
    f.write(reinterpret_cast<const char*>(b.data()), b.size());
}

// ─────────────────────────────────────────────────────────────────────────────
SegmentWriter::SegmentWriter(const std::string& dir, uint32_t segment_id)
    : dir_(dir), seg_id_(segment_id)
{}

std::string SegmentWriter::path(const std::string& ext) const {
    return dir_ + "/_" + std::to_string(seg_id_) + "." + ext;
}

// _N.tim_title, _N.doc_body, _N.pos_content …
std::string SegmentWriter::fieldPath(const std::string& field,
                                      const std::string& ext) const {
    return dir_ + "/_" + std::to_string(seg_id_) + "." + ext + "_" + field;
}

// ─────────────────────────────────────────────────────────────────────────────
// BM25 辅助
// ─────────────────────────────────────────────────────────────────────────────

float SegmentWriter::calcMaxTfNorm(const PostingList& pl) {
    const float k1 = 1.2f;
    float max_norm = 0.0f;
    for (const auto& e : pl.entries()) {
        float tf   = static_cast<float>(e.tf);
        float norm = tf * (k1 + 1.0f) / (tf + k1);
        max_norm   = std::max(max_norm, norm);
    }
    return max_norm;
}

std::vector<float> SegmentWriter::calcBlockMaxTfNorms(const PostingList& pl) {
    const float k1 = 1.2f;
    constexpr int BLOCK_SIZE = 128;
    const auto& entries = pl.entries();
    std::vector<float> result;
    result.reserve((entries.size() + BLOCK_SIZE - 1) / BLOCK_SIZE);
    for (size_t i = 0; i < entries.size(); i += BLOCK_SIZE) {
        size_t end = std::min(i + (size_t)BLOCK_SIZE, entries.size());
        float max_norm = 0.0f;
        for (size_t j = i; j < end; ++j) {
            float tf   = static_cast<float>(entries[j].tf);
            float norm = tf * (k1 + 1.0f) / (tf + k1);
            max_norm   = std::max(max_norm, norm);
        }
        result.push_back(max_norm);
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// flush：主入口（per-field 版本）
// ─────────────────────────────────────────────────────────────────────────────

SegmentWriteStats SegmentWriter::flush(
    const std::map<std::string, InMemoryIndex>& field_indexes,
    const std::vector<StoredDoc>&               stored_docs,
    uint32_t                                    total_docs,
    const std::map<std::string, float>&         field_avg_doc_lens,
    const Schema&                               schema)
{
    using Clock = std::chrono::steady_clock;
    namespace fs = std::filesystem;

    SegmentWriteStats stats;
    stats.segment_id = seg_id_;
    stats.doc_count  = total_docs;

    std::vector<std::string> written_fields;

    for (const auto& [field, idx] : field_indexes) {
        if (idx.termCount() == 0) continue;
        written_fields.push_back(field);

        FieldIndexStats fstats;
        fstats.avg_doc_len = field_avg_doc_lens.count(field)
            ? field_avg_doc_lens.at(field) : 0.f;
        fstats.term_count = static_cast<uint32_t>(idx.termCount());

        std::map<std::string, TermMeta> term_dict;

        // 1. 写 .tim_<field>
        {
            auto t = Clock::now();
            writeFieldTim(field, idx, term_dict, stats);
            stats.tim_us    += std::chrono::duration_cast<
                                   std::chrono::microseconds>(Clock::now() - t).count();
            fstats.tim_bytes = fs::file_size(fieldPath(field, "tim"));
            stats.tim_bytes += fstats.tim_bytes;
        }

        // 2. 写 .doc_<field>
        {
            auto t = Clock::now();
            writeFieldDoc(field, idx, term_dict, stats);
            stats.doc_us    += std::chrono::duration_cast<
                                   std::chrono::microseconds>(Clock::now() - t).count();
            fstats.doc_bytes = fs::file_size(fieldPath(field, "doc"));
            stats.doc_bytes += fstats.doc_bytes;
        }

        // 3. 写 .pos_<field>（仅 FreqsPositions）
        const FieldSchema* fschema = schema.find(field);
        bool write_pos = fschema && (fschema->index == IndexOption::FreqsPositions);
        if (write_pos) {
            auto t = Clock::now();
            writeFieldPos(field, idx, term_dict);
            stats.pos_us    += std::chrono::duration_cast<
                                   std::chrono::microseconds>(Clock::now() - t).count();
            fstats.pos_bytes = fs::file_size(fieldPath(field, "pos"));
            stats.pos_bytes += fstats.pos_bytes;
        }

        stats.field_stats[field] = fstats;
        stats.term_count        += fstats.term_count;
    }

    // 4. 写 .fdt / .fdx
    {
        auto t = Clock::now();
        std::vector<uint64_t> fdx_offsets;
        writeFdt(stored_docs, fdx_offsets);
        writeFdx(fdx_offsets);
        stats.fdt_fdx_us = std::chrono::duration_cast<
                               std::chrono::microseconds>(Clock::now() - t).count();
        if (fs::exists(path("fdt"))) stats.fdt_bytes = fs::file_size(path("fdt"));
        if (fs::exists(path("fdx"))) stats.fdx_bytes = fs::file_size(path("fdx"));
    }

    // 5. 写 .liv
    writeLiv(total_docs);
    stats.liv_bytes = fs::file_size(path("liv"));

    // 6. 写 .si（含 indexed_fields 列表）
    {
        SegmentInfo si;
        si.segment_id = seg_id_;
        si.doc_count  = total_docs;
        si.term_count = stats.term_count;
        time_t now = time(nullptr);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", localtime(&now));
        si.created_at = tbuf;
        std::string fields_str;
        for (size_t i = 0; i < written_fields.size(); ++i) {
            if (i > 0) fields_str += ',';
            fields_str += written_fields[i];
        }
        si.indexed_fields = fields_str;
        writeSi(si);
        stats.si_bytes = fs::file_size(path("si"));
    }

    // 聚合统计
    if (stats.term_count > 0) {
        stats.avg_pl_df               = static_cast<float>(stats.total_pl_entries) / stats.term_count;
        stats.avg_skip_nodes_per_term = static_cast<float>(stats.total_skip_nodes)  / stats.term_count;
    }

    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// writeFieldTim：写 _N.tim_<field>
//
// 格式（与旧 .tim 相同）：
//   4B  term_count
//   for each term (字母序):
//     [str] term
//     4B    df
//     4B    total_tf
//     8B    posting_offset（待 writeFieldDoc 填入，此处先写 0）
//     8B    skip_offset（同上）
//     8B    pos_offset（待 writeFieldPos 填入）
//     4B    upper_bound
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeFieldTim(
    const std::string&               field,
    const InMemoryIndex&             idx,
    std::map<std::string, TermMeta>& term_dict_out,
    SegmentWriteStats&               stats)
{
    auto terms = idx.sortedTerms();
    std::ofstream f(fieldPath(field, "tim"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .tim_" + field + ": " + fieldPath(field, "tim"));

    writeU32(f, static_cast<uint32_t>(terms.size()));

    for (const auto& term : terms) {
        const PostingList* pl = idx.getPostingList(term);
        if (!pl) continue;

        float    max_tf_norm = calcMaxTfNorm(*pl);
        uint32_t df          = static_cast<uint32_t>(pl->size());

        // 聚合 df 统计（跨字段累积）
        stats.total_pl_entries += df;
        if (df > stats.max_pl_df) {
            stats.max_pl_df   = df;
            stats.max_pl_term = field + ":" + term;
        }

        TermMeta meta;
        meta.doc_freq        = df;
        meta.total_term_freq = pl->totalTermFreq();
        meta.posting_offset  = 0;
        meta.skip_offset     = 0;
        meta.pos_offset      = 0;
        meta.upper_bound     = max_tf_norm;

        term_dict_out[term] = meta;

        writeStr(f, term);
        writeU32(f, meta.doc_freq);
        writeU32(f, meta.total_term_freq);
        writeU64(f, 0);
        writeU64(f, 0);
        writeU64(f, 0);
        writeF32(f, max_tf_norm);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writeFieldDoc：写 _N.doc_<field>，并回写 posting_offset/skip_offset 到 _N.tim_<field>
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeFieldDoc(
    const std::string&               field,
    const InMemoryIndex&             idx,
    std::map<std::string, TermMeta>& term_dict,
    SegmentWriteStats&               stats)
{
    std::ofstream f(fieldPath(field, "doc"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .doc_" + field + ": " + fieldPath(field, "doc"));

    uint32_t term_count = static_cast<uint32_t>(term_dict.size());
    writeU32(f, term_count);

    auto terms = idx.sortedTerms();
    for (const auto& term : terms) {
        const PostingList* pl = idx.getPostingList(term);
        if (!pl) continue;

        auto& meta = term_dict[term];

        std::vector<DocId>    doc_ids   = pl->docIds();
        std::vector<float>    block_ubs = calcBlockMaxTfNorms(*pl);
        std::vector<SkipNode> skip_nodes;
        std::vector<uint8_t>  compressed = PForDelta::compress(doc_ids, skip_nodes, block_ubs);

        uint32_t sn_count = static_cast<uint32_t>(skip_nodes.size());
        stats.total_skip_nodes += sn_count;
        if (sn_count > stats.max_skip_nodes) {
            stats.max_skip_nodes = sn_count;
            stats.max_skip_term  = field + ":" + term;
        }

        SkipList sl(skip_nodes);
        std::vector<uint8_t> sl_bytes = sl.serialize();

        uint64_t current_pos    = static_cast<uint64_t>(f.tellp());
        meta.skip_offset        = current_pos;
        meta.posting_offset     = current_pos + static_cast<uint64_t>(sl_bytes.size());

        writeBytes(f, sl_bytes);
        writeBytes(f, compressed);
    }

    // 回写 posting_offset / skip_offset 到 _N.tim_<field>
    {
        std::ofstream ftim(fieldPath(field, "tim"), std::ios::binary | std::ios::trunc);
        writeU32(ftim, term_count);
        for (const auto& term : terms) {
            const PostingList* pl2 = idx.getPostingList(term);
            if (!pl2) continue;
            const auto& m = term_dict[term];
            writeStr(ftim, term);
            writeU32(ftim, m.doc_freq);
            writeU32(ftim, m.total_term_freq);
            writeU64(ftim, m.posting_offset);
            writeU64(ftim, m.skip_offset);
            writeU64(ftim, m.pos_offset);
            writeF32(ftim, m.upper_bound);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writeFieldPos：写 _N.pos_<field>，并回写 pos_offset 到 _N.tim_<field>
//
// 格式（每个 term）：
//   for each doc in posting list:
//     4B  doc_id
//     4B  tf
//     tf × 4B  positions
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeFieldPos(
    const std::string&               field,
    const InMemoryIndex&             idx,
    std::map<std::string, TermMeta>& term_dict)
{
    std::ofstream f(fieldPath(field, "pos"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .pos_" + field + ": " + fieldPath(field, "pos"));

    auto terms = idx.sortedTerms();
    for (const auto& term : terms) {
        const PostingList* pl = idx.getPostingList(term);
        if (!pl) continue;

        term_dict[term].pos_offset = static_cast<uint64_t>(f.tellp());

        for (const auto& entry : pl->entries()) {
            writeU32(f, entry.doc_id);
            writeU32(f, entry.tf);
            for (Pos p : entry.positions)
                writeU32(f, p);
        }
    }

    // 回写 pos_offset 到 _N.tim_<field>
    {
        uint32_t term_count = static_cast<uint32_t>(term_dict.size());
        std::ofstream ftim(fieldPath(field, "tim"), std::ios::binary | std::ios::trunc);
        writeU32(ftim, term_count);
        for (const auto& term : terms) {
            const PostingList* pl2 = idx.getPostingList(term);
            if (!pl2) continue;
            const auto& m = term_dict[term];
            writeStr(ftim, term);
            writeU32(ftim, m.doc_freq);
            writeU32(ftim, m.total_term_freq);
            writeU64(ftim, m.posting_offset);
            writeU64(ftim, m.skip_offset);
            writeU64(ftim, m.pos_offset);
            writeF32(ftim, m.upper_bound);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writeFdt：文档原文
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeFdt(
    const std::vector<StoredDoc>& docs,
    std::vector<uint64_t>&        offsets_out)
{
    std::ofstream f(path("fdt"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .fdt");

    offsets_out.resize(docs.size());
    for (size_t i = 0; i < docs.size(); ++i) {
        offsets_out[i] = static_cast<uint64_t>(f.tellp());
        writeU32(f, docs[i].doc_id);
        writeU64(f, docs[i].ext_id);
        writeU32(f, static_cast<uint32_t>(docs[i].str_fields.size()));
        for (const auto& [name, val] : docs[i].str_fields) {
            writeStr(f, name);
            writeStr(f, val);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writeFdx：文档存储索引
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeFdx(const std::vector<uint64_t>& offsets) {
    std::ofstream f(path("fdx"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .fdx");

    writeU32(f, static_cast<uint32_t>(offsets.size()));
    for (size_t i = 0; i < offsets.size(); ++i) {
        writeU32(f, static_cast<uint32_t>(i + 1));  // doc_id 从 1 开始
        writeU64(f, offsets[i]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writeLiv：存活文档位图（bitset，初始全 1）
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeLiv(uint32_t doc_count) {
    std::ofstream f(path("liv"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .liv");

    writeU32(f, doc_count);
    uint32_t bytes = (doc_count + 7) / 8;
    std::vector<uint8_t> bitmap(bytes, 0xFF);
    if (doc_count % 8 != 0) {
        uint8_t mask = (1u << (doc_count % 8)) - 1u;
        bitmap.back() &= mask;
    }
    f.write(reinterpret_cast<const char*>(bitmap.data()), bytes);
}

// ─────────────────────────────────────────────────────────────────────────────
// writeSi：Segment 元数据
//
// 格式：4B segment_id | 4B doc_count | 4B term_count | str created_at | str indexed_fields
// SegmentReader 读取 indexed_fields 以发现 _N.tim_<field> 文件；
// 旧格式（无 indexed_fields）在 SegmentReader 侧回退扫目录。
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeSi(const SegmentInfo& info) {
    std::ofstream f(path("si"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .si");

    writeU32(f, info.segment_id);
    writeU32(f, info.doc_count);
    writeU32(f, info.term_count);
    writeStr(f, info.created_at);
    writeStr(f, info.indexed_fields);
}

} // namespace ii
