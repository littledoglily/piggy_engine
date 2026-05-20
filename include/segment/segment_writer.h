#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// segment_writer.h  —  将内存倒排索引 flush 为 Segment 文件集
//
// 输出文件（前缀 _N.xxx_<field>）：
//   _N.tim_<field>   Term 词典（per-field）
//   _N.doc_<field>   Posting List（SkipList + PForDelta Block，per-field）
//   _N.pos_<field>   位置信息（仅 FreqsPositions 字段，per-field）
//   _N.fdt           文档原文存储（全字段共享）
//   _N.fdx           文档存储索引（doc_id → fdt 字节偏移）
//   _N.liv           存活文档位图（初始全 1）
//   _N.si            Segment 元数据（含 indexed_fields 字段列表）
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include "postings/posting_list.h"
#include "schema/schema.h"
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>

namespace ii {

// ── per-field 索引统计 ────────────────────────────────────────────────────────
struct FieldIndexStats {
    uint32_t term_count  = 0;
    uint64_t tim_bytes   = 0;
    uint64_t doc_bytes   = 0;
    uint64_t pos_bytes   = 0;   // 0 表示 FreqsOnly（无 .pos_<field> 文件）
    float    avg_doc_len = 0.f;
};

// ── Segment 构建统计（flush 完成后返回）──────────────────────────────────────
struct SegmentWriteStats {
    uint32_t    segment_id  = 0;
    uint32_t    doc_count   = 0;
    uint32_t    term_count  = 0;   // 所有字段 term 数之和

    // Posting List 聚合（跨字段）
    uint64_t    total_pl_entries = 0;
    uint32_t    max_pl_df        = 0;
    std::string max_pl_term;
    float       avg_pl_df        = 0.f;

    // SkipNode 聚合（跨字段）
    uint64_t    total_skip_nodes         = 0;
    uint32_t    max_skip_nodes           = 0;
    std::string max_skip_term;
    float       avg_skip_nodes_per_term  = 0.f;

    // 文件大小（字节）—— per-field 文件之和
    uint64_t    tim_bytes  = 0;
    uint64_t    doc_bytes  = 0;
    uint64_t    pos_bytes  = 0;
    uint64_t    fdt_bytes  = 0;
    uint64_t    fdx_bytes  = 0;
    uint64_t    liv_bytes  = 0;
    uint64_t    si_bytes   = 0;

    // 写文件耗时（微秒）—— per-field 累加
    uint64_t    tim_us     = 0;
    uint64_t    doc_us     = 0;
    uint64_t    pos_us     = 0;
    uint64_t    fdt_fdx_us = 0;

    // per-field 细粒度统计
    std::map<std::string, FieldIndexStats> field_stats;

    uint64_t totalSegBytes() const {
        return tim_bytes + doc_bytes + pos_bytes +
               fdt_bytes + fdx_bytes + liv_bytes + si_bytes;
    }
    uint64_t totalSegUs() const {
        return tim_us + doc_us + pos_us + fdt_fdx_us;
    }
};

// ── Segment 元数据（写入 .si 文件）──────────────────────────────────────────
// 格式：4B segment_id | 4B doc_count | 4B term_count | str created_at | str indexed_fields
struct SegmentInfo {
    uint32_t    segment_id;
    uint32_t    doc_count;
    uint32_t    term_count;
    std::string created_at;
    std::string indexed_fields;   // 逗号分隔的字段名，供 SegmentReader 发现 per-field 文件
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

    // 主入口：将 per-field InMemoryIndex 和文档原文 flush 到磁盘，返回构建统计
    // FastField 由 IndexWriter 独立管理（调用 FastFieldWriter::flush）
    SegmentWriteStats flush(
        const std::map<std::string, InMemoryIndex>& field_indexes,
        const std::vector<StoredDoc>&               stored_docs,
        uint32_t                                    total_docs,
        const std::map<std::string, float>&         field_avg_doc_lens,
        const Schema&                               schema
    );

private:
    // ── per-field 倒排写入 ───────────────────────────────────────────────────
    void writeFieldTim(const std::string& field, const InMemoryIndex& idx,
                       const std::map<DocId, uint32_t>& doc_lens, float avgdl,
                       std::map<std::string, TermMeta>& dict_out,
                       SegmentWriteStats& stats);

    void writeFieldDoc(const std::string& field, const InMemoryIndex& idx,
                       const std::map<DocId, uint32_t>& doc_lens,
                       std::map<std::string, TermMeta>& dict,
                       SegmentWriteStats& stats);

    void writeFieldPos(const std::string& field, const InMemoryIndex& idx,
                       std::map<std::string, TermMeta>& dict);

    // ── 文档存储 / 元数据 ────────────────────────────────────────────────────
    void writeFdt(const std::vector<StoredDoc>& docs,
                  std::vector<uint64_t>& offsets_out);

    void writeFdx(const std::vector<uint64_t>& offsets);

    void writeLiv(uint32_t doc_count);

    void writeSi(const SegmentInfo& info);

    // ── BM25 辅助 ────────────────────────────────────────────────────────────
    // 计算字段内每个 doc 的 token 数（doc_id → length）
    static std::map<DocId, uint32_t> computeFieldDocLens(const InMemoryIndex& idx);

    // max tf_norm（不含 IDF），b=0.75 完整 BM25；avgdl=0 退化为 b=0
    static float calcMaxTfNorm(const PostingList& pl,
                               const std::map<DocId, uint32_t>& doc_lens,
                               float avgdl);

    static std::vector<float> calcBlockMaxTfNorms(const PostingList& pl,
                                                  const std::map<DocId, uint32_t>& doc_lens,
                                                  float avgdl);

    // 写 _N.len_<field>：uint16_t[total_docs]，索引 = doc_id - 1
    void writeFieldLen(const std::string& field,
                       const std::map<DocId, uint32_t>& doc_lens,
                       uint32_t total_docs);

    // ── 路径辅助 ─────────────────────────────────────────────────────────────
    // path("fdt") → dir_/_N.fdt
    std::string path(const std::string& ext) const;
    // fieldPath("title", "tim") → dir_/_N.tim_title
    std::string fieldPath(const std::string& field, const std::string& ext) const;

    std::string  dir_;
    uint32_t     seg_id_;
};

} // namespace ii
