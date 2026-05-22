#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index/segment_writer.h  —  将内存倒排索引 flush 为 Segment 文件集
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "store/posting_list.h"
#include "field/schema.h"
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>

namespace ii {

struct FieldIndexStats {
    uint32_t term_count  = 0;
    uint64_t tim_bytes   = 0;
    uint64_t doc_bytes   = 0;
    uint64_t pos_bytes   = 0;
    float    avg_doc_len = 0.f;
};

struct SegmentWriteStats {
    uint32_t    segment_id  = 0;
    uint32_t    doc_count   = 0;
    uint32_t    term_count  = 0;
    uint64_t    total_pl_entries     = 0;
    uint32_t    max_pl_df            = 0;
    std::string max_pl_term;
    float       avg_pl_df            = 0.f;
    uint64_t    total_skip_nodes     = 0;
    uint32_t    max_skip_nodes       = 0;
    std::string max_skip_term;
    float       avg_skip_nodes_per_term = 0.f;
    uint64_t    tim_bytes  = 0;
    uint64_t    doc_bytes  = 0;
    uint64_t    pos_bytes  = 0;
    uint64_t    fdt_bytes  = 0;
    uint64_t    fdx_bytes  = 0;
    uint64_t    liv_bytes  = 0;
    uint64_t    si_bytes   = 0;
    uint64_t    tim_us     = 0;
    uint64_t    doc_us     = 0;
    uint64_t    pos_us     = 0;
    uint64_t    fdt_fdx_us = 0;
    std::map<std::string, FieldIndexStats> field_stats;

    uint64_t totalSegBytes() const {
        return tim_bytes + doc_bytes + pos_bytes +
               fdt_bytes + fdx_bytes + liv_bytes + si_bytes;
    }
    uint64_t totalSegUs() const { return tim_us + doc_us + pos_us + fdt_fdx_us; }
};

struct SegmentInfo {
    uint32_t    segment_id;
    uint32_t    doc_count;
    uint32_t    term_count;
    std::string created_at;
    std::string indexed_fields;
};

struct StoredDoc {
    DocId       doc_id  = 0;
    uint64_t    ext_id  = 0;
    std::unordered_map<std::string, std::string> str_fields;
    size_t     ramUsage() const {
        size_t use = sizeof(doc_id) + sizeof(ext_id);
        for (const auto& [name, val] : str_fields) {
            use += name.size() + val.size();
        }
        return use;
    }
};

class SegmentWriter {
public:
    explicit SegmentWriter(const std::string& dir, uint32_t segment_id);

    SegmentWriteStats flush(
        const std::map<std::string, InMemoryIndex>& field_indexes,
        const std::vector<StoredDoc>&               stored_docs,
        uint32_t                                    total_docs,
        const std::map<std::string, float>&         field_avg_doc_lens,
        const Schema&                               schema
    );

private:
    void writeFieldTim(const std::string& field, const InMemoryIndex& idx,
                       const std::map<DocId, uint32_t>& doc_lens, float avgdl,
                       std::map<std::string, TermMeta>& dict_out, SegmentWriteStats& stats);

    void writeFieldDoc(const std::string& field, const InMemoryIndex& idx,
                       const std::map<DocId, uint32_t>& doc_lens,
                       std::map<std::string, TermMeta>& dict, SegmentWriteStats& stats);

    void writeFieldPos(const std::string& field, const InMemoryIndex& idx,
                       std::map<std::string, TermMeta>& dict);

    void writeFdt(const std::vector<StoredDoc>& docs, std::vector<uint64_t>& offsets_out);
    void writeFdx(const std::vector<uint64_t>& offsets);
    void writeLiv(uint32_t doc_count);
    void writeSi (const SegmentInfo& info);

    static std::map<DocId, uint32_t> computeFieldDocLens(const InMemoryIndex& idx);
    static float calcMaxTfNorm(const PostingList& pl,
                               const std::map<DocId, uint32_t>& doc_lens, float avgdl);
    static std::vector<float> calcBlockMaxTfNorms(const PostingList& pl,
                                                  const std::map<DocId, uint32_t>& doc_lens,
                                                  float avgdl);
    void writeFieldLen(const std::string& field,
                       const std::map<DocId, uint32_t>& doc_lens, uint32_t total_docs);

    std::string path     (const std::string& ext) const;
    std::string fieldPath(const std::string& field, const std::string& ext) const;

    std::string dir_;
    uint32_t    seg_id_;
};

} // namespace ii
