#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// segment_merger.h  —  Segment 合并 + 软删除管理
//
// 解决的问题：
//   1. 写入过程中每次 flush 产生一个小 Segment，查询时需要同时搜索所有
//      Segment 并归并结果，Segment 数量越多开销越大。
//   2. 删除/更新文档后，旧文档占用磁盘空间但只是在 .liv 中标记为 0，
//      只有合并时才能真正清除。
//
// 合并流程（两个 Segment → 一个新 Segment）：
//   Step1  读取所有源 Segment 的 .tim 词典，建立合并词表
//   Step2  对每个 term，多路归并 posting list
//           - 跳过 .liv=0 的已删除 doc
//           - doc_id 重新从 1 连续编号（去掉空洞）
//   Step3  重新 Delta 编码 + PForDelta 压缩
//   Step4  重建 SkipList
//   Step5  写新 Segment 的所有文件
//   Step6  原子更新 segments_N 注册表
//   Step7  删除旧 Segment 文件
//
// 软删除流程：
//   - 修改 .liv 位图中对应 bit 为 0（内存 + 持久化）
//   - 合并时自动跳过已删除 doc
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include "segment_reader.h"
#include "segment_writer.h"
#include <string>
#include <vector>
#include <memory>

namespace ii {

// ── 合并策略 ──────────────────────────────────────────────────────────────────
enum class MergePolicy {
    FORCE,          // 强制合并所有 Segment 为一个
    TIERED,         // 分层合并：Segment 数量超过阈值时合并最小的几个
};

// ── 合并结果统计 ──────────────────────────────────────────────────────────────
struct MergeStats {
    uint32_t input_segment_count;   // 参与合并的 Segment 数量
    uint32_t input_doc_count;       // 合并前总文档数（含已删除）
    uint32_t deleted_doc_count;     // 被真正清除的已删除文档数
    uint32_t output_doc_count;      // 合并后存活文档数
    uint32_t output_segment_id;     // 新 Segment 的 ID
    size_t   input_bytes;           // 合并前磁盘占用（近似）
    size_t   output_bytes;          // 合并后磁盘占用（近似）
};

class SegmentMerger {
public:
    explicit SegmentMerger(const std::string& dir);

    // ── 软删除接口 ────────────────────────────────────────────────────────────

    // 软删除指定 doc_id（在所有 Segment 中搜索并标记）
    // 返回 true 表示找到并标记成功
    bool softDelete(DocId doc_id);

    // 批量软删除
    int softDeleteBatch(const std::vector<DocId>& doc_ids);

    // 查询某 doc_id 是否存活
    bool isAlive(DocId doc_id) const;

    // ── 合并接口 ──────────────────────────────────────────────────────────────

    // 强制合并：将所有 Segment 合并为一个
    MergeStats mergeAll(uint32_t new_segment_id);

    // 按策略合并（TIERED：Segment 数 > max_segments 时触发）
    MergeStats mergeIfNeeded(MergePolicy policy = MergePolicy::TIERED,
                              int max_segments = 5);

    // ── Segment 注册表管理 ───────────────────────────────────────────────────

    // 读取当前所有有效 Segment ID
    std::vector<uint32_t> activeSegmentIds() const;

    // 打印 Segment 统计信息
    void printStats() const;

private:
    // 核心合并：将 src_ids 对应的 Segment 合并为 new_segment_id
    MergeStats doMerge(const std::vector<uint32_t>& src_ids,
                       uint32_t new_segment_id);

    // 多路归并两个 posting list（跳过已删除 doc）
    // doc_id_remap: 旧 doc_id → 新 doc_id（去空洞后重新编号）
    std::vector<DocId> mergePostingLists(
        const std::vector<std::vector<DocId>>& lists,
        const std::vector<SegmentReader*>&     readers,
        const std::unordered_map<uint32_t, DocId>& remap  // seg_local_id → global_new_id
    );

    // 删除 Segment 的所有文件
    void deleteSegmentFiles(uint32_t seg_id);

    // 更新 segments_N 注册表
    void writeSegmentsFile(const std::vector<uint32_t>& active_ids,
                           uint32_t generation);

    // 文件路径辅助
    std::string segPath(uint32_t seg_id, const std::string& ext) const;
    std::string dir_;

    // 已加载的 SegmentReader（软删除时需要）
    std::vector<std::unique_ptr<SegmentReader>> readers_;
    std::vector<uint32_t>                       seg_ids_;
};

} // namespace ii
