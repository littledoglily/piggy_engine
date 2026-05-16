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

// ─────────────────────────────────────────────────────────────────────────────
// BM25 辅助
// ─────────────────────────────────────────────────────────────────────────────

float SegmentWriter::calcMaxTfNorm(const PostingList& pl) {
    // max_tf_norm = max over all docs { tf × (k1+1) / (tf + k1) }
    // 使用 dl=avgdl 简化（k1=1.2，b 项消去），结果不依赖全局 N 和 IDF
    const float k1 = 1.2f;
    float max_norm = 0.0f;
    for (const auto& e : pl.entries()) {
        float tf   = static_cast<float>(e.tf);
        float norm = tf * (k1 + 1.0f) / (tf + k1);
        max_norm   = std::max(max_norm, norm);
    }
    return max_norm;
}

// ─────────────────────────────────────────────────────────────────────────────
// flush：主入口
// ─────────────────────────────────────────────────────────────────────────────

SegmentWriteStats SegmentWriter::flush(
    const InMemoryIndex&          mem_index,
    const std::vector<StoredDoc>& stored_docs,
    uint32_t                      total_docs,
    float                         avg_doc_len)
{
    using Clock = std::chrono::steady_clock;
    namespace fs = std::filesystem;

    SegmentWriteStats stats;
    stats.segment_id = seg_id_;
    stats.doc_count  = total_docs;

    // 1. 写 .tim
    std::map<std::string, TermMeta> term_dict;
    {
        auto t = Clock::now();
        writeTim(mem_index, term_dict, stats);
        stats.tim_us    = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t).count();
        stats.tim_bytes = fs::file_size(path("tim"));
    }

    // 2. 写 .doc
    {
        auto t = Clock::now();
        writeDoc(mem_index, term_dict, stats);
        stats.doc_us    = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t).count();
        stats.doc_bytes = fs::file_size(path("doc"));
    }

    // 3. 写 .pos
    {
        auto t = Clock::now();
        writePos(mem_index, term_dict);
        stats.pos_us    = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t).count();
        stats.pos_bytes = fs::file_size(path("pos"));
    }

    // 4. 写 .fdt / .fdx
    {
        auto t = Clock::now();
        std::vector<uint64_t> fdx_offsets;
        writeFdt(stored_docs, fdx_offsets);
        writeFdx(fdx_offsets);
        stats.fdt_fdx_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t).count();
        stats.fdt_bytes  = fs::file_size(path("fdt"));
        stats.fdx_bytes  = fs::file_size(path("fdx"));
    }

    // 5. 写 .liv
    writeLiv(total_docs);
    stats.liv_bytes = fs::file_size(path("liv"));

    // 6. 写 .si
    SegmentInfo si;
    si.segment_id = seg_id_;
    si.doc_count  = total_docs;
    si.term_count = static_cast<uint32_t>(mem_index.termCount());
    time_t now = time(nullptr);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", localtime(&now));
    si.created_at = tbuf;
    writeSi(si);
    stats.si_bytes = fs::file_size(path("si"));

    // 聚合 posting list 统计（在 writeTim 中已收集 term_count / total_pl_entries / max_pl_df）
    if (stats.term_count > 0) {
        stats.avg_pl_df             = static_cast<float>(stats.total_pl_entries) / stats.term_count;
        stats.avg_skip_nodes_per_term = static_cast<float>(stats.total_skip_nodes) / stats.term_count;
    }

    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// writeTim：Term 词典
//
// 格式：
//   4B  term_count
//   for each term (字母序):
//     [str] term
//     4B    df
//     4B    total_tf
//     8B    posting_offset（待 writeDoc 填入，此处先写 0）
//     8B    skip_offset（同上）
//     8B    pos_offset（待 writePos 填入）
//     4B    upper_bound
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeTim(
    const InMemoryIndex&            idx,
    std::map<std::string, TermMeta>& term_dict_out,
    SegmentWriteStats&               stats)
{
    auto terms = idx.sortedTerms();
    std::ofstream f(path("tim"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .tim: " + path("tim"));

    writeU32(f, static_cast<uint32_t>(terms.size()));
    stats.term_count = static_cast<uint32_t>(terms.size());

    for (const auto& term : terms) {
        const PostingList* pl = idx.getPostingList(term);
        if (!pl) continue;

        float max_tf_norm = calcMaxTfNorm(*pl);
        uint32_t df = static_cast<uint32_t>(pl->size());

        // 聚合 df 统计
        stats.total_pl_entries += df;
        if (df > stats.max_pl_df) {
            stats.max_pl_df   = df;
            stats.max_pl_term = term;
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
// writeDoc：Posting List（SkipList + PForDelta Block）
//
// 格式（每个 term）：
//   [SkipList 序列化字节]  （长度由 SkipList::serializedSize() 决定）
//   [PForDelta Block 字节序列]
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeDoc(
    const InMemoryIndex&            idx,
    std::map<std::string, TermMeta>& term_dict,
    SegmentWriteStats&               stats)
{
    std::ofstream f(path("doc"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .doc: " + path("doc"));

    uint32_t term_count = static_cast<uint32_t>(term_dict.size());
    writeU32(f, term_count);

    auto terms = idx.sortedTerms();
    for (const auto& term : terms) {
        const PostingList* pl = idx.getPostingList(term);
        if (!pl) continue;

        auto& meta = term_dict[term];

        std::vector<DocId> doc_ids = pl->docIds();
        std::vector<SkipNode> skip_nodes;
        std::vector<uint8_t> compressed = PForDelta::compress(doc_ids, skip_nodes, meta.upper_bound);

        // 聚合 skipnode 统计
        uint32_t sn_count = static_cast<uint32_t>(skip_nodes.size());
        stats.total_skip_nodes += sn_count;
        if (sn_count > stats.max_skip_nodes) {
            stats.max_skip_nodes = sn_count;
            stats.max_skip_term  = term;
        }

        // 构建并序列化 SkipList
        SkipList sl(skip_nodes);
        std::vector<uint8_t> sl_bytes = sl.serialize();

        // 记录偏移（当前文件位置）
        uint64_t current_pos = static_cast<uint64_t>(f.tellp());
        meta.skip_offset     = current_pos;
        meta.posting_offset  = current_pos + static_cast<uint64_t>(sl_bytes.size());

        // 写 SkipList
        writeBytes(f, sl_bytes);
        // 写 PForDelta 数据
        writeBytes(f, compressed);
    }

    // 回写 posting_offset / skip_offset 到 .tim 文件
    // （简化实现：重新打开 .tim，顺序覆写 offset 字段）
    // 实际 Lucene 是两趟写：先写 .doc，再写 .tim；这里用临时更新
    {
        // 重新读 .tim，更新 offset，重写
        std::ifstream fin(path("tim"), std::ios::binary);
        std::vector<uint8_t> tim_data(
            (std::istreambuf_iterator<char>(fin)),
            std::istreambuf_iterator<char>()
        );
        fin.close();

        // 解析并重写（简化：重新生成整个 .tim）
        std::ofstream ftim(path("tim"), std::ios::binary | std::ios::trunc);
        uint32_t tcount = static_cast<uint32_t>(term_dict.size());
        writeU32(ftim, tcount);
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
// writePos：位置信息
//
// 格式（每个 term）：
//   for each doc in posting list:
//     4B  doc_id
//     4B  tf
//     tf × 4B  positions
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writePos(
    const InMemoryIndex&            idx,
    std::map<std::string, TermMeta>& term_dict)
{
    std::ofstream f(path("pos"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .pos: " + path("pos"));

    auto terms = idx.sortedTerms();
    for (const auto& term : terms) {
        const PostingList* pl = idx.getPostingList(term);
        if (!pl) continue;

        // 记录 pos_offset
        uint64_t pos_off = static_cast<uint64_t>(f.tellp());
        term_dict[term].pos_offset = pos_off;

        for (const auto& entry : pl->entries()) {
            writeU32(f, entry.doc_id);
            writeU32(f, entry.tf);
            for (Pos p : entry.positions) {
                writeU32(f, p);
            }
        }
    }

    // 回写 pos_offset 到 .tim（合并到 writeDoc 的回写逻辑中）
    {
        std::ofstream ftim(path("tim"), std::ios::binary | std::ios::trunc);
        uint32_t tcount = static_cast<uint32_t>(term_dict.size());
        writeU32(ftim, tcount);
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
//
// 格式（每篇文档）：
//   4B  doc_id (uint32)
//   8B  ext_id (uint64)   外部数字 ID
//   [str] source          外部字符串标识
//   [str] title
//   [str] body
//   [str] category
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
        writeStr(f, docs[i].source);
        writeStr(f, docs[i].title);
        writeStr(f, docs[i].body);
        writeStr(f, docs[i].category);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writeFdx：文档存储索引
//
// 格式：
//   4B  doc_count
//   doc_count × (4B doc_id + 8B fdt_offset)
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
//
// 格式：
//   4B  doc_count
//   ceil(doc_count/8) 字节的位图
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeLiv(uint32_t doc_count) {
    std::ofstream f(path("liv"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .liv");

    writeU32(f, doc_count);
    uint32_t bytes = (doc_count + 7) / 8;
    std::vector<uint8_t> bitmap(bytes, 0xFF);  // 全 1：所有 doc 存活
    // 最后一个字节的高位可能超出 doc_count，清零
    if (doc_count % 8 != 0) {
        uint8_t mask = (1u << (doc_count % 8)) - 1u;
        bitmap.back() &= mask;
    }
    f.write(reinterpret_cast<const char*>(bitmap.data()), bytes);
}

// ─────────────────────────────────────────────────────────────────────────────
// writeSi：Segment 元数据
// ─────────────────────────────────────────────────────────────────────────────

void SegmentWriter::writeSi(const SegmentInfo& info) {
    std::ofstream f(path("si"), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot open .si");

    writeU32(f, info.segment_id);
    writeU32(f, info.doc_count);
    writeU32(f, info.term_count);
    writeStr(f, info.created_at);
}

} // namespace ii
