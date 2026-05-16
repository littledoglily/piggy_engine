#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index_writer.h  —  文档写入入口
//
// 职责：
//   1. 接收文档，按 Schema 路由各字段
//   2. 文本/Keyword 字段 → Analyzer → InMemoryIndex（倒排）
//   3. FastField 字段 → FastFieldWriter（数值列存）
//   4. Stored 字段 → stored_docs_buf_（.fdt）
//   5. Buffer 满时触发 flush → SegmentWriter + FastFieldWriter::flush
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include "schema/schema.h"
#include "tokenizer/analyzer.h"
#include "postings/posting_list.h"
#include "segment/segment_writer.h"
#include "fastfield/fast_field_writer.h"
#include <string>
#include <vector>
#include <memory>

namespace ii {

class IndexWriter {
public:
    // Schema 不传则使用 defaultSchema()（与原有硬编码行为等价）
    IndexWriter(const std::string& dir, float ram_buffer_mb = 16.0f,
                Schema schema = Schema::defaultSchema());
    ~IndexWriter();

    void addDocument(const Document& doc);
    void deleteDocument(DocId doc_id);
    void flush();
    void commit();

    uint32_t totalDocs()    const { return total_docs_; }
    uint32_t segmentCount() const { return next_seg_id_; }

private:
    size_t estimateRamUsage() const;

    // 从 Document 按字段名取值（过渡桥接，保持 Document 为具名结构体）
    static std::string getDocText  (const Document& doc, const std::string& name);
    static int64_t     getDocInt64 (const Document& doc, const std::string& name);
    static float       getDocFloat32(const Document& doc, const std::string& name);

    std::string   dir_;
    float         ram_buffer_mb_;
    Schema        schema_;

    Analyzer      analyzer_;
    InMemoryIndex mem_index_;

    std::vector<StoredDoc> stored_docs_buf_;
    FastFieldWriter        ff_writer_;

    uint32_t total_docs_   = 0;
    uint32_t next_seg_id_  = 0;
    uint64_t total_tokens_ = 0;
};

} // namespace ii
