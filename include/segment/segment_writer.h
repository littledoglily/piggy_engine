#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// segment_writer.h  —  将内存倒排索引 flush 为 Segment 文件集
//
// 输出文件（前缀 _N.xxx）：
//   _N.tim   Term 词典（FST 简化版，实现为有序 map 的二进制序列化）
//   _N.doc   Posting List（SkipList + PForDelta Block）
//   _N.pos   位置信息（每个 term 每个 doc 的词序位置列表）
//   _N.fdt   文档原文存储（简单拼接，实际 Lucene 用 LZ4 Chunk 压缩）
//   _N.fdx   文档存储索引（doc_id → fdt 字节偏移）
//   _N.liv   存活文档位图（初始全 1）
//   _N.si    Segment 元数据
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include "postings/posting_list.h"
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>

namespace ii {

// ── Segment 构建统计（flush 完成后返回）──────────────────────────────────────
struct SegmentWriteStats {
    uint32_t    segment_id  = 0;
    uint32_t    doc_count   = 0;
    uint32_t    term_count  = 0;

    // Posting List 聚合
    uint64_t    total_pl_entries = 0;  // Σ df
    uint32_t    max_pl_df        = 0;
    std::string max_pl_term;
    float       avg_pl_df        = 0.f;

    // SkipNode 聚合
    uint64_t    total_skip_nodes         = 0;
    uint32_t    max_skip_nodes           = 0;
    std::string max_skip_term;
    float       avg_skip_nodes_per_term  = 0.f;

    // 文件大小（字节）
    uint64_t    tim_bytes  = 0;
    uint64_t    doc_bytes  = 0;
    uint64_t    pos_bytes  = 0;
    uint64_t    fdt_bytes  = 0;
    uint64_t    fdx_bytes  = 0;
    uint64_t    liv_bytes  = 0;
    uint64_t    si_bytes   = 0;

    // 写文件耗时（微秒）
    uint64_t    tim_us     = 0;
    uint64_t    doc_us     = 0;
    uint64_t    pos_us     = 0;
    uint64_t    fdt_fdx_us = 0;

    uint64_t totalSegBytes() const {
        return tim_bytes + doc_bytes + pos_bytes +
               fdt_bytes + fdx_bytes + liv_bytes + si_bytes;
    }
    uint64_t totalSegUs() const {
        return tim_us + doc_us + pos_us + fdt_fdx_us;
    }
};

// ── Segment 元数据（写入 .si 文件）──────────────────────────────────────────
struct SegmentInfo {
    uint32_t    segment_id;
    uint32_t    doc_count;
    uint32_t    term_count;
    std::string created_at;   // ISO 时间字符串
};

// ── 文档存储条目（写入 .fdt/.fdx）────────────────────────────────────────────
// 格式：4B doc_id | 8B ext_id | 4B field_count | (str name | str value) × N
struct StoredDoc {
    DocId       doc_id  = 0;
    uint64_t    ext_id  = 0;
    std::unordered_map<std::string, std::string> str_fields;
};

class SegmentWriter {
public:
    explicit SegmentWriter(const std::string& dir, uint32_t segment_id);

    // 主入口：将内存索引、文档原文一次性 flush 到磁盘，返回构建统计
    // FastField 由 IndexWriter 独立管理（调用 FastFieldWriter::flush）
    SegmentWriteStats flush(
        const InMemoryIndex&          mem_index,
        const std::vector<StoredDoc>& stored_docs,
        uint32_t                      total_docs,
        float                         avg_doc_len
    );

private:
    // ── 写各个文件 ───────────────────────────────────────────────────────────
    void writeTim(const InMemoryIndex& idx,
                  std::map<std::string, TermMeta>& term_dict_out,
                  SegmentWriteStats& stats);

    void writeDoc(const InMemoryIndex& idx,
                  std::map<std::string, TermMeta>& term_dict,
                  SegmentWriteStats& stats);

    void writePos(const InMemoryIndex& idx,
                  std::map<std::string, TermMeta>& term_dict);

    void writeFdt(const std::vector<StoredDoc>& docs,
                  std::vector<uint64_t>& offsets_out);

    void writeFdx(const std::vector<uint64_t>& offsets);

    void writeLiv(uint32_t doc_count);

    void writeSi(const SegmentInfo& info);

    // 计算整条 posting list 的 max_tf_norm（用于 TermMeta.upper_bound）
    static float calcMaxTfNorm(const PostingList& pl);

    // 按 128-doc Block 分组，逐 Block 计算 max_tf_norm（用于 BlockHeader.max_score）
    static std::vector<float> calcBlockMaxTfNorms(const PostingList& pl);

    // 文件路径辅助
    std::string path(const std::string& ext) const;

    std::string  dir_;
    uint32_t     seg_id_;
};

} // namespace ii
