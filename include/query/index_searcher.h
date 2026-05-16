#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index_searcher.h  —  检索入口
//
// 支持：
//   AND 查询（Zigzag 双指针求交集）
//   OR  查询（WAND 上界剪枝，返回 TopK）
//   跨多个 Segment 搜索，结果归并
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include "tokenizer/analyzer.h"
#include "segment/segment_reader.h"
#include <vector>
#include <string>
#include <memory>
#include <queue>
#include <numeric>

namespace ii {

enum class QueryMode {
    AND,   // 所有 term 必须出现（Zigzag 交集）
    OR,    // 任意 term 出现（WAND TopK）
};

class IndexSearcher {
public:
    // 打开索引目录，加载所有 Segment
    explicit IndexSearcher(const std::string& dir);

    // 搜索接口
    // query：查询字符串（空格分隔）
    // top_k：返回前 K 个结果
    // mode：AND 或 OR
    std::vector<SearchResult> search(
        const std::string& query,
        int                top_k = 10,
        QueryMode          mode  = QueryMode::AND
    ) const;

    // 打印搜索结果
    static void printResults(const std::vector<SearchResult>& results);

private:
    // AND 查询：Zigzag 双指针求交集，对交集计算 BM25
    std::vector<SearchResult> searchAND(
        const std::vector<std::string>& terms,
        int top_k,
        const SegmentReader& seg
    ) const;

    // OR 查询：WAND 算法（上界剪枝，返回 TopK）
    std::vector<SearchResult> searchOR_WAND(
        const std::vector<std::string>& terms,
        int top_k,
        const SegmentReader& seg
    ) const;

    // 跨 Segment 归并 TopK
    std::vector<SearchResult> mergeTopK(
        std::vector<std::vector<SearchResult>>& per_seg_results,
        int top_k
    ) const;

    // ── WAND 内部：优先队列比较器 ────────────────────────────────────────────
    struct HeapEntry {
        float score;
        DocId doc_id;
        bool operator<(const HeapEntry& o) const { return score > o.score; }
    };
    using MinHeap = std::priority_queue<HeapEntry>;

    Analyzer analyzer_;
    std::vector<std::unique_ptr<SegmentReader>> segments_;
};

} // namespace ii
