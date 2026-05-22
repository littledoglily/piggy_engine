#include "index/segment_writer.h"
#include "codec/pfor_delta.h"
#include "codec/skiplist.h"
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

// <dir>/segment_N/<ext>
std::string SegmentWriter::path(const std::string& ext) const {
    return dir_ + "/segment_" + std::to_string(seg_id_) + "/" + ext;
}

// <dir>/segment_N/<ext>_<field>  (e.g. segment_0/tim_body)
std::string SegmentWriter::fieldPath(const std::string& field,
                                      const std::string& ext) const {
    return dir_ + "/segment_" + std::to_string(seg_id_) + "/" + ext + "_" + field;
}

// ─────────────────────────────────────────────────────────────────────────────
// BM25 辅助
// ─────────────────────────────────────────────────────────────────────────────

std::unordered_map<DocId, uint32_t> SegmentWriter::computeFieldDocLens(const InMemoryIndex& idx) {
    std::unordered_map<DocId, uint32_t> lens;
    for (const auto& [_, pl] : idx.postingLists()) {
        for (const auto& e : pl.entries())
            lens[e.doc_id] += e.tf;
    }
    return lens;
}

SegmentWriter::TfNormMap SegmentWriter::calcTfNorms(
    const InMemoryIndex&                         idx,
    const std::unordered_map<DocId, uint32_t>&   doc_lens,
    float                                        avgdl)
{
    const float k1 = 1.2f, b = 0.75f;
    constexpr size_t BLOCK_SIZE = 128;

    TfNormMap result;
    for (const auto& [term, pl] : idx.postingLists()) {
        const auto& entries = pl.entries();
        float global_max = 0.0f;
        std::vector<float> block_norms;
        block_norms.reserve((entries.size() + BLOCK_SIZE - 1) / BLOCK_SIZE);

        for (size_t i = 0; i < entries.size(); i += BLOCK_SIZE) {
            size_t end = std::min(i + BLOCK_SIZE, entries.size());
            float block_max = 0.0f;
            for (size_t j = i; j < end; ++j) {
                const auto& e = entries[j];
                float tf  = static_cast<float>(e.tf);
                float dl  = (avgdl > 0.f && doc_lens.count(e.doc_id))
                            ? static_cast<float>(doc_lens.at(e.doc_id)) : avgdl;
                float norm = tf * (k1 + 1.0f) /
                             (tf + k1 * (1.0f - b + b * (avgdl > 0.f ? dl / avgdl : 1.0f)));
                block_max = std::max(block_max, norm);
            }
            block_norms.push_back(block_max);
            global_max = std::max(global_max, block_max);
        }
        result.emplace(term, std::make_pair(global_max, std::move(block_norms)));
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

    // 创建 segment 子目录并标记为写入中
    const std::string seg_dir = dir_ + "/segment_" + std::to_string(seg_id_);
    fs::create_directories(seg_dir);
    { std::ofstream ing(seg_dir + "/.ing"); }   // 创建 .ing 占位文件

    SegmentWriteStats stats;
    stats.segment_id = seg_id_;
    stats.doc_count  = total_docs;

    std::vector<std::string> written_fields;

    for (const auto& [field, idx] : field_indexes) {
        if (idx.termCount() == 0) continue;
        written_fields.push_back(field);

        float avgdl = field_avg_doc_lens.count(field)
                      ? field_avg_doc_lens.at(field) : 0.f;

        FieldIndexStats fstats;
        fstats.avg_doc_len = avgdl;
        fstats.term_count  = static_cast<uint32_t>(idx.termCount());

        // per-doc 字段长度（doc_id → token 数），用于 b=0.75 BM25
        auto doc_lens = computeFieldDocLens(idx);

        // 一次遍历所有 posting，同时算出 global max 和 per-block max
        TfNormMap tf_norms = calcTfNorms(idx, doc_lens, avgdl);

        std::map<std::string, TermMeta>            term_dict;
        std::unordered_map<std::string, uint64_t>  tim_patch;

        // 1. 写 .tim_<field>
        {
            auto t = Clock::now();
            writeFieldTim(field, idx, avgdl, term_dict, stats, tim_patch, tf_norms);
            stats.tim_us    += std::chrono::duration_cast<
                                   std::chrono::microseconds>(Clock::now() - t).count();
            fstats.tim_bytes = fs::file_size(fieldPath(field, "tim"));
            stats.tim_bytes += fstats.tim_bytes;
        }

        // 2. 写 .doc_<field>（含 tf 字节，seekp 回填 posting/skip/tf_data offset）
        {
            auto t = Clock::now();
            writeFieldDoc(field, idx, term_dict, stats, tim_patch, tf_norms);
            stats.doc_us    += std::chrono::duration_cast<
                                   std::chrono::microseconds>(Clock::now() - t).count();
            fstats.doc_bytes = fs::file_size(fieldPath(field, "doc"));
            stats.doc_bytes += fstats.doc_bytes;
        }

        // 3. 写 .pos_<field>（仅 FreqsPositions），seekp 回填 pos_offset
        const FieldSchema* fschema = schema.find(field);
        bool write_pos = fschema && (fschema->index == IndexOption::FreqsPositions);
        if (write_pos) {
            auto t = Clock::now();
            writeFieldPos(field, idx, term_dict, tim_patch);
            stats.pos_us    += std::chrono::duration_cast<
                                   std::chrono::microseconds>(Clock::now() - t).count();
            fstats.pos_bytes = fs::file_size(fieldPath(field, "pos"));
            stats.pos_bytes += fstats.pos_bytes;
        }

        // 4. 写 .len_<field>（per-doc 字段长度，uint16_t 数组）
        writeFieldLen(field, doc_lens, total_docs);

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

    // 所有文件写完：写 .done，移除 .ing
    { std::ofstream done(seg_dir + "/.done"); }
    fs::remove(seg_dir + "/.ing");

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
    float                            avgdl,
    std::map<std::string, TermMeta>& term_dict_out,
    SegmentWriteStats&               stats,
    std::unordered_map<std::string, uint64_t>& tim_patch_out,
    const TfNormMap&                 tf_norms)
{
    const auto& pls = idx.postingLists();
    std::ofstream f(fieldPath(field, "tim"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .tim_" + field + ": " + fieldPath(field, "tim"));

    writeU32(f, static_cast<uint32_t>(pls.size()));

    for (const auto& [term, pl] : pls) {
        float    max_tf_norm = tf_norms.at(term).first;
        uint32_t df          = static_cast<uint32_t>(pl.size());

        stats.total_pl_entries += df;
        if (df > stats.max_pl_df) {
            stats.max_pl_df   = df;
            stats.max_pl_term = field + ":" + term;
        }

        TermMeta meta;
        meta.doc_freq        = df;
        meta.total_term_freq = pl.totalTermFreq();
        meta.posting_offset  = 0;
        meta.skip_offset     = 0;
        meta.pos_offset      = 0;
        meta.upper_bound     = max_tf_norm;
        meta.tf_data_offset  = 0;

        term_dict_out[term] = meta;

        writeStr(f, term);
        writeU32(f, meta.doc_freq);
        writeU32(f, meta.total_term_freq);
        // 记录此处为 offsets 块起始：seekp 回填时的基址
        // layout: posting_offset(8) skip_offset(8) pos_offset(8) upper_bound(4) tf_data_offset(8)
        tim_patch_out[term] = static_cast<uint64_t>(f.tellp());
        writeU64(f, 0);   // posting_offset  (+0)
        writeU64(f, 0);   // skip_offset     (+8)
        writeU64(f, 0);   // pos_offset      (+16)
        writeF32(f, max_tf_norm);
        writeU64(f, 0);   // tf_data_offset  (+28)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writeFieldDoc：写 _N.doc_<field>，并回写 posting_offset/skip_offset 到 _N.tim_<field>
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeFieldDoc(
    const std::string&               field,
    const InMemoryIndex&             idx,
    std::map<std::string, TermMeta>& term_dict,
    SegmentWriteStats&               stats,
    const std::unordered_map<std::string, uint64_t>& tim_patch,
    const TfNormMap&                 tf_norms)
{
    std::ofstream f(fieldPath(field, "doc"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .doc_" + field + ": " + fieldPath(field, "doc"));

    const auto& pls = idx.postingLists();
    uint32_t term_count = static_cast<uint32_t>(pls.size());
    writeU32(f, term_count);

    for (const auto& [term, pl] : pls) {
        auto& meta = term_dict[term];

        std::vector<DocId>        doc_ids   = pl.docIds();
        const std::vector<float>& block_ubs = tf_norms.at(term).second;
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

        uint64_t current_pos = static_cast<uint64_t>(f.tellp());
        meta.skip_offset     = current_pos;
        meta.posting_offset  = current_pos + static_cast<uint64_t>(sl_bytes.size());

        writeBytes(f, sl_bytes);
        writeBytes(f, compressed);

        // tf 字节数组：每个 doc 一个 uint8_t（saturate at 255），按 posting 顺序
        meta.tf_data_offset = static_cast<uint64_t>(f.tellp());
        for (const auto& e : pl.entries()) {
            // 这个地方是对工程存储和效果的取舍，取舍逻辑是k=1.2时，tf=255时的BM25贡献分已经非常接近于 tf=inf 的情况了（约为99.6%），因此认为超过255的tf对排名贡献差别不大，直接饱和处理可以节省存储空间
            uint8_t tf_byte = static_cast<uint8_t>(std::min((uint32_t)e.tf, 255u));
            f.write(reinterpret_cast<const char*>(&tf_byte), 1);
        }
    }

    // seekp 回填 posting_offset / skip_offset / tf_data_offset 到 _N.tim_<field>
    // layout: posting_offset(+0,8B) skip_offset(+8,8B) pos_offset(+16,8B) upper_bound(+24,4B) tf_data_offset(+28,8B)
    {
        std::fstream ftim(fieldPath(field, "tim"),
                          std::ios::binary | std::ios::in | std::ios::out);
        for (const auto& [term, pl] : pls) {
            const auto& m    = term_dict[term];
            uint64_t    base = tim_patch.at(term);
            ftim.seekp(static_cast<std::streamoff>(base));
            ftim.write(reinterpret_cast<const char*>(&m.posting_offset), 8);
            ftim.write(reinterpret_cast<const char*>(&m.skip_offset),    8);
            // pos_offset(+16) 留给 writeFieldPos 回填，跳过
            ftim.seekp(static_cast<std::streamoff>(base + 28));
            ftim.write(reinterpret_cast<const char*>(&m.tf_data_offset), 8);
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
    std::map<std::string, TermMeta>& term_dict,
    const std::unordered_map<std::string, uint64_t>& tim_patch)
{
    std::ofstream f(fieldPath(field, "pos"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .pos_" + field + ": " + fieldPath(field, "pos"));

    const auto& pls = idx.postingLists();
    for (const auto& [term, pl] : pls) {
        term_dict[term].pos_offset = static_cast<uint64_t>(f.tellp());

        for (const auto& entry : pl.entries()) {
            writeU32(f, entry.doc_id);
            writeU32(f, entry.tf);
            for (Pos p : entry.positions)
                writeU32(f, p);
        }
    }

    // seekp 回填 pos_offset 到 _N.tim_<field>（+16 处）
    {
        std::fstream ftim(fieldPath(field, "tim"),
                          std::ios::binary | std::ios::in | std::ios::out);
        for (const auto& [term, pl] : pls) {
            uint64_t pos_off = term_dict[term].pos_offset;
            ftim.seekp(static_cast<std::streamoff>(tim_patch.at(term) + 16));
            ftim.write(reinterpret_cast<const char*>(&pos_off), 8);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writeFieldLen：写 _N.len_<field>（per-doc 字段长度，uint16_t 数组）
// 格式：4B doc_count | doc_count × 2B field_length（doc_id 1-indexed，索引 = doc_id-1）
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeFieldLen(const std::string& field,
                                  const std::unordered_map<DocId, uint32_t>& doc_lens,
                                  uint32_t total_docs)
{
    std::ofstream f(fieldPath(field, "len"), std::ios::binary | std::ios::trunc);
    if (!f) return;  // 非致命，不影响其他文件

    writeU32(f, total_docs);
    for (uint32_t i = 0; i < total_docs; ++i) {
        DocId did = i + 1;  // 1-indexed
        auto it = doc_lens.find(did);
        uint16_t len = (it != doc_lens.end())
                       ? static_cast<uint16_t>(std::min(it->second, 65535u))
                       : 0u;
        f.write(reinterpret_cast<const char*>(&len), 2);
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
