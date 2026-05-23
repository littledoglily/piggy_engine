#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// memory/memory_segment_reader.h  —  实时索引只读接口（Step 4）
//
// 职责：
//   将 MemorySegment 的 arena + hashtable 暴露为 ISegmentReader 接口，
//   使 IndexSearcher 能将内存 segment 纳入查询路径。
//
// 关键设计：
//   - postingIterator() 遍历 TermPage 链，按 doc_id 排序后构造内存模式
//     PostingIterator（不需要文件 I/O）。
//   - getTermMeta() 从 hashtable Bucket 计算 df/ttf，并推算 IDF/upper_bound。
//   - isAlive() 始终返回 true（内存 segment 无软删除）。
//   - segmentId() 返回 MEM_SEG_ID（0xFFFFFFFE），区别于磁盘 segment。
//
// 线程安全：
//   MemorySegment 写入停止后（flush 前）可安全并发读取。
//   写入期间的并发读写由上层 epoch-snapshot 机制（Step 6）保证。
// ─────────────────────────────────────────────────────────────────────────────
#include "memory/memory_segment.h"
#include "index/i_segment_reader.h"
#include "store/i_posting_iterator.h"
#include "core/types.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ii::memory {

class MemorySegmentReader : public ii::ISegmentReader {
public:
    static constexpr uint32_t MEM_SEG_ID = 0xFFFFFFFEu;

    // 非并发用途：MemorySegment 必须比此 Reader 存活更长（调用方保证）
    explicit MemorySegmentReader(const MemorySegment& seg);

    // SWMR 用途（Step 7）：共享 MemorySegment 所有权，保证 arena 在所有 Reader 释放后才销毁
    explicit MemorySegmentReader(std::shared_ptr<const MemorySegment> seg);

    // ── ISegmentReader ────────────────────────────────────────────────────────

    std::unique_ptr<ii::IPostingIterator> postingIterator(
        const std::string& field, const std::string& term) const override;

    const ii::TermMeta* getTermMeta(const std::string& field,
                                     const std::string& term) const override;

    uint32_t fieldDocLen   (const std::string& field, ii::DocId doc_id) const override;
    float    fieldAvgDocLen(const std::string& field) const override;

    uint32_t docCount()  const override { return seg_.docCount(); }
    uint32_t segmentId() const override { return MEM_SEG_ID; }

    const std::vector<std::string>& indexedFieldNames() const override {
        return field_names_;
    }

    bool isAlive(ii::DocId) const override { return true; }

    ii::StoredDocResult readStoredDoc(ii::DocId doc_id) const override;

private:
    static constexpr float BM25_K1 = 1.2f;
    static constexpr float BM25_B  = 0.75f;

    // 当以 shared_ptr 构造时持有所有权，保证 MemorySegment 生命周期
    std::shared_ptr<const MemorySegment> owned_seg_;
    const MemorySegment& seg_;
    std::vector<std::string> field_names_;

    // Cache computed TermMeta. Protected by mutex for concurrent reader threads.
    mutable std::mutex term_meta_mutex_;
    mutable std::unordered_map<std::string, ii::TermMeta> term_meta_cache_;
};

} // namespace ii::memory
