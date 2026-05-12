#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index_writer.h  —  文档写入入口
//
// 职责：
//   1. 接收文档，调用 Analyzer 产出 Token 流
//   2. 写入 InMemoryIndex（内存倒排链）
//   3. Buffer 满时触发 flush → SegmentWriter
//   4. 维护全局 doc_count 和 avg_doc_len
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include "analyzer.h"
#include "posting_list.h"
#include "segment_writer.h"
#include <string>
#include <vector>
#include <memory>

namespace ii {

class IndexWriter {
public:
    // dir：索引目录；ram_buffer_mb：内存 Buffer 上限（触发 flush）
    IndexWriter(const std::string& dir, float ram_buffer_mb = 16.0f);
    ~IndexWriter();

    // 写入一篇文档
    void addDocument(const Document& doc);

    // 删除文档（软删除，标记 .liv）
    void deleteDocument(DocId doc_id);

    // 强制 flush 当前 Buffer 到磁盘
    void flush();

    // commit：flush + 持久化（更新 segments_N 文件）
    void commit();

    uint32_t totalDocs()    const { return total_docs_; }
    uint32_t segmentCount() const { return next_seg_id_; }

private:
    // 估算当前内存 Buffer 大小（字节）
    size_t estimateRamUsage() const;

    std::string   dir_;
    float         ram_buffer_mb_;

    Analyzer      analyzer_;
    InMemoryIndex mem_index_;

    std::vector<StoredDoc> stored_docs_buf_;  // 待 flush 的原文
    uint32_t               total_docs_   = 0;
    uint32_t               next_seg_id_  = 0;
    uint64_t               total_tokens_ = 0;  // 用于计算 avg_doc_len
};

} // namespace ii
