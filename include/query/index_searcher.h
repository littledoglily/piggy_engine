#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index_searcher.h  —  检索入口
//
// 支持：
//   AND 查询（Zigzag 双指针求交集）
//   OR  查询（WAND 上界剪枝，返回 TopK）
//   field:term 语法（只在指定字段的 term 空间搜索）
//   bare term 展开（AND：所有默认字段；OR：任意默认字段）
//   跨多个 Segment 搜索，结果归并
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include "tokenizer/analyzer.h"
#include "segment/segment_reader.h"
#include "postings/posting_iterator.h"
#include "query/query.h"
#include "query/query_parser.h"
#include "query/wand_scorer.h"
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

// 解析后的查询单元
// field = "" 表示 bare term（按模式展开到所有默认字段）
struct FieldTerm {
    std::string field;  // "" = bare，"title" = field:term
    std::string term;   // Analyzer 分析后的词
};

class IndexSearcher {
public:
    // 打开索引目录，加载所有 Segment
    explicit IndexSearcher(const std::string& dir);

    // 搜索接口（支持 "title:python machine" 语法）
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
    // ── 查询解析 ─────────────────────────────────────────────────────────────
    // "title:python machine" → [{field="title",term="python"}, {field="",term="machine"}]
    std::vector<FieldTerm> parseQuery(const std::string& raw_query) const;

    // ── IDF 计算 ──────────────────────────────────────────────────────────────
    // key: "field:term"（field-qualified）或裸 "term"（bare term 及 legacy fallback）
    // bare term 会为每个默认字段都计算一次，key 为 "field:term"
    std::unordered_map<std::string, float> computeFieldTermIdfs(
        const std::vector<FieldTerm>& fterms
    ) const;

    // ── Scorer 树路径（Step 8 新增）────────────────────────────────────────
    // 从 BooleanQuery 树收集所有 (field,term) 对并计算 IDF
    std::unordered_map<std::string, float> computeIdfsFromBQ(
        const BooleanQuery& bq) const;

    // 驱动单个 Segment 上的 Scorer 树，返回 top_k 结果
    std::vector<SearchResult> searchImpl(
        const BooleanQuery& bq,
        int top_k,
        const SegmentReader& seg,
        const std::unordered_map<std::string, float>& term_idfs,
        const NumericFilter* filter
    ) const;

    // ── AND 查询：Zigzag 双指针求交集（保留，已由 searchImpl 替代）─────────
    [[deprecated("use searchImpl via BooleanQuery")]]
    std::vector<SearchResult> searchAND(
        const std::vector<FieldTerm>& fterms,
        const std::unordered_map<std::string, float>& term_idfs,
        int top_k,
        const SegmentReader& seg,
        const NumericFilter* filter
    ) const;

    // ── OR 查询：WAND 算法（保留，已由 searchImpl 替代）─────────────────────
    [[deprecated("use searchImpl via BooleanQuery")]]
    std::vector<SearchResult> searchOR_WAND(
        const std::vector<FieldTerm>& fterms,
        const std::unordered_map<std::string, float>& term_idfs,
        int top_k,
        const SegmentReader& seg,
        const NumericFilter* filter
    ) const;

    // ── 打分辅助 ─────────────────────────────────────────────────────────────
    // 对 doc_id 计算所有 expanded FieldTerms 的 per-field BM25 之和
    float scoreDoc(DocId doc_id,
                   const std::vector<FieldTerm>& expanded,
                   const std::unordered_map<std::string, float>& term_idfs,
                   const SegmentReader& seg) const;

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
                        const std::vector<FieldTerm>& fterms,
                        const std::unordered_map<std::string, float>& term_idfs) const;

    // ── WAND 内部：优先队列比较器 ────────────────────────────────────────────
    struct HeapEntry {
        float score;
        DocId doc_id;
        bool operator<(const HeapEntry& o) const { return score > o.score; }
    };
    using MinHeap = std::priority_queue<HeapEntry>;

    Analyzer    analyzer_;
    std::vector<std::unique_ptr<SegmentReader>> segments_;
    uint32_t    global_total_docs_ = 0;
    mutable bool debug_ = false;

    // 默认搜索字段：构造时从第一个 Segment 的 indexedFieldNames() 初始化
    // 为空时退回 legacy 模式（merged term_dict_）
    std::vector<std::string> default_search_fields_;
};

} // namespace ii
