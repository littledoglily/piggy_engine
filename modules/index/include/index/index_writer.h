#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index/index_writer.h  —  文档写入入口
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "field/schema.h"
#include "field/field_descriptor.h"
#include "analysis/analyzer.h"
#include "store/posting_list.h"
#include "index/segment_writer.h"
#include "field/fast_field_writer.h"
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>

namespace ii {

class IndexWriter {
public:
    IndexWriter(const std::string& dir, float ram_buffer_mb = 16.0f,
                Schema schema = Schema::defaultSchema());
    ~IndexWriter();

    void addDocument   (const Document& doc);
    void deleteDocument(DocId doc_id);
    void flush();
    void commit();

    uint32_t totalDocs()    const { return total_docs_; }
    uint32_t segmentCount() const { return next_seg_id_; }

    const std::vector<SegmentWriteStats>& segStatsHistory() const { return seg_stats_history_; }
    const std::vector<FFWriteStats>&      ffStatsHistory()  const { return ff_stats_history_; }

private:
    size_t estimateRamUsage() const;
    static void printFlushStats(const SegmentWriteStats& seg, const FFWriteStats& ff);

    std::string   dir_;
    float         ram_buffer_mb_;
    Schema        schema_;
    std::vector<std::unique_ptr<FieldDescriptor>> descriptors_;

    Analyzer      analyzer_;

    std::map<std::string, InMemoryIndex> field_indexes_;
    std::map<std::string, uint64_t>      field_token_counts_;

    std::vector<StoredDoc> stored_docs_buf_;
    FastFieldWriter        ff_writer_;

    uint32_t total_docs_  = 0;
    uint32_t next_seg_id_ = 0;

    std::vector<SegmentWriteStats> seg_stats_history_;
    std::vector<FFWriteStats>      ff_stats_history_;
};

} // namespace ii
