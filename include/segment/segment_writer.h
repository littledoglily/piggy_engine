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

namespace ii {

// ── Segment 元数据（写入 .si 文件）──────────────────────────────────────────
struct SegmentInfo {
    uint32_t    segment_id;
    uint32_t    doc_count;
    uint32_t    term_count;
    std::string created_at;   // ISO 时间字符串
};

// ── 文档存储条目（写入 .fdt/.fdx）────────────────────────────────────────────
struct StoredDoc {
    DocId       doc_id  = 0;
    uint64_t    ext_id  = 0;     // 外部数字 ID
    std::string source;          // 外部字符串标识（URL / ISBN / 路径等）
    std::string title;
    std::string body;
    std::string category;
};

class SegmentWriter {
public:
    explicit SegmentWriter(const std::string& dir, uint32_t segment_id);

    // 主入口：将内存索引、文档原文一次性 flush 到磁盘
    // FastField 由 IndexWriter 独立管理（调用 FastFieldWriter::flush）
    void flush(
        const InMemoryIndex&          mem_index,
        const std::vector<StoredDoc>& stored_docs,
        uint32_t                      total_docs,
        float                         avg_doc_len  // BM25 归一化用
    );

private:
    // ── 写各个文件 ───────────────────────────────────────────────────────────
    void writeTim(const InMemoryIndex& idx,
                  std::map<std::string, TermMeta>& term_dict_out,
                  uint32_t total_docs, float avg_doc_len);

    void writeDoc(const InMemoryIndex& idx,
                  std::map<std::string, TermMeta>& term_dict,
                  float avg_doc_len, uint32_t total_docs);

    void writePos(const InMemoryIndex& idx,
                  std::map<std::string, TermMeta>& term_dict);

    void writeFdt(const std::vector<StoredDoc>& docs,
                  std::vector<uint64_t>& offsets_out);

    void writeFdx(const std::vector<uint64_t>& offsets);

    void writeLiv(uint32_t doc_count);

    void writeSi(const SegmentInfo& info);

    // 计算 BM25 IDF（简化）
    static float calcIdf(uint32_t df, uint32_t total_docs);

    // 计算 term 的 UB（Upper Bound）
    static float calcUB(const PostingList& pl,
                        float idf, float avg_doc_len);

    // 文件路径辅助
    std::string path(const std::string& ext) const;

    std::string  dir_;
    uint32_t     seg_id_;
};

} // namespace ii
