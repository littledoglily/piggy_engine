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
#include "postings/posting_iterator.h"
#include <vector>
#include <string>
#include <unordered_map>
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

    // 带数值过滤的搜索（FastField in-filter）
    std::vector<SearchResult> search(
        const std::string&   query,
        const NumericFilter& filter,
        int                  top_k = 10,
        QueryMode            mode  = QueryMode::AND
    ) const;

    // 打印搜索结果
    static void printResults(const std::vector<SearchResult>& results);

    // 开启调试输出：IDF 全局信息、SkipNode 列表、UB 验证
    void setDebug(bool v) { debug_ = v; }

private:
    // 基于全局统计为每个 query term 计算 {term -> global_idf}
    std::unordered_map<std::string, float> computeTermIdfs(
        const std::vector<std::string>& terms
    ) const;

    // AND 查询：Zigzag 双指针求交集，对交集计算 BM25
    std::vector<SearchResult> searchAND(
        const std::vector<std::string>& terms,
        const std::unordered_map<std::string, float>& term_idfs,
        int top_k,
        const SegmentReader& seg,
        const NumericFilter* filter
    ) const;

    // OR 查询：WAND 算法（上界剪枝，返回 TopK）
    std::vector<SearchResult> searchOR_WAND(
        const std::vector<std::string>& terms,
        const std::unordered_map<std::string, float>& term_idfs,
        int top_k,
        const SegmentReader& seg,
        const NumericFilter* filter
    ) const;

    // 对单个 doc（by local_doc_idx = doc_id-1）做数值过滤检查
    bool passesFilter(const SegmentReader& seg,
                      DocId doc_id,
                      const NumericFilter& filter) const;

    // 跨 Segment 归并 TopK
    std::vector<SearchResult> mergeTopK(
        std::vector<std::vector<SearchResult>>& per_seg_results,
        int top_k
    ) const;

    // 调试：打印每个 term 的 SkipNode 列表 + UB 验证（--debug 模式）
    void printTermDebug(const SegmentReader& seg,
                        const std::vector<std::string>& terms,
                        const std::unordered_map<std::string, float>& term_idfs) const;

    // ── WAND 内部：优先队列比较器 ────────────────────────────────────────────
    struct HeapEntry {
        float score;
        DocId doc_id;
        bool operator<(const HeapEntry& o) const { return score > o.score; }
    };
    using MinHeap = std::priority_queue<HeapEntry>;

    Analyzer analyzer_;
    std::vector<std::unique_ptr<SegmentReader>> segments_;
    uint32_t global_total_docs_ = 0;
    mutable bool debug_ = false;
};

} // namespace ii
