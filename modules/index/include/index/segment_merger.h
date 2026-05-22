#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index/segment_merger.h  —  Segment 合并 + 软删除管理
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "index/segment_reader.h"
#include "index/segment_writer.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace ii {

enum class MergePolicy { FORCE, TIERED };

struct MergeStats {
    uint32_t input_segment_count;
    uint32_t input_doc_count;
    uint32_t deleted_doc_count;
    uint32_t output_doc_count;
    uint32_t output_segment_id;
    size_t   input_bytes;
    size_t   output_bytes;
};

class SegmentMerger {
public:
    // 扫描目录，加载所有 .si 文件对应的 Segment
    explicit SegmentMerger(const std::string& dir);

    // 仅加载指定 seg_ids 的 Segment（K-way 合并用）
    SegmentMerger(const std::string& dir, const std::vector<uint32_t>& seg_ids);

    bool softDelete(DocId doc_id);
    int  softDeleteBatch(const std::vector<DocId>& doc_ids);
    bool isAlive(DocId doc_id) const;

    MergeStats mergeAll(uint32_t new_segment_id);
    MergeStats mergeIfNeeded(MergePolicy policy = MergePolicy::TIERED,
                              int max_segments = 5);

    std::vector<uint32_t> activeSegmentIds() const;
    void printStats() const;

private:
    MergeStats doMerge(const std::vector<uint32_t>& src_ids, uint32_t new_segment_id);

    std::vector<DocId> mergePostingLists(
        const std::vector<std::vector<DocId>>& lists,
        const std::vector<SegmentReader*>&     readers,
        const std::unordered_map<uint32_t, DocId>& remap
    );

    void deleteSegmentFiles(uint32_t seg_id);
    void writeSegmentsFile(const std::vector<uint32_t>& active_ids, uint32_t generation);

    std::string segPath     (uint32_t seg_id, const std::string& ext) const;
    std::string segFieldPath(uint32_t seg_id, const std::string& field,
                             const std::string& ext) const;

    std::string dir_;
    std::vector<std::unique_ptr<SegmentReader>> readers_;
    std::vector<uint32_t>                       seg_ids_;
};

} // namespace ii
