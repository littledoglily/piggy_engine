#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query/index_searcher.h  —  检索入口
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "analysis/analyzer.h"
#include "index/i_segment_reader.h"
#include "index/segment_reader.h"
#include "query/query.h"
#include "query/query_parser.h"
#include "query/wand_scorer.h"
#include <memory>
#include <numeric>
#include <queue>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ii {

enum class QueryMode {
    AND,
    OR,
};

struct FieldTerm {
    std::string field;
    std::string term;
};

class IndexSearcher {
public:
    explicit IndexSearcher(const std::string& dir);

    // ── 实时 segment 接入（Step 6/7）────────────────────────────────────────
    //
    // attachRealtime：将内存 segment 纳入查询。调用方负责保证 shared_ptr 的生命周期
    // 覆盖 MemorySegment 的 arena，在所有持有该快照的 search() 返回后方可 reset()。
    // detachRealtime：移除实时 segment（用于 flush 后切换到磁盘 segment）。
    void attachRealtime(std::shared_ptr<const ISegmentReader> rt);
    void detachRealtime();

    std::vector<SearchResult> search(
        const std::string& query,
        int                top_k = 10,
        QueryMode          mode  = QueryMode::AND
    ) const;

    std::vector<SearchResult> search(
        const std::string&   query,
        const NumericFilter& filter,
        int                  top_k = 10,
        QueryMode            mode  = QueryMode::AND
    ) const;

    static void printResults(const std::vector<SearchResult>& results);

    void setDebug(bool v) { debug_ = v; }

private:
    std::vector<FieldTerm> parseQuery(const std::string& raw_query) const;

    std::unordered_map<std::string, float> computeIdfsFromBQ(
        const BooleanQuery& bq,
        const ISegmentReader* rt  // nullptr if no realtime segment
    ) const;

    // deprecated path のみ使用（searchAND / searchOR_WAND）
    std::unordered_map<std::string, float> computeFieldTermIdfs(
        const std::vector<FieldTerm>& fterms
    ) const;

    // 在单个 Segment（disk 或 memory）上驱动 Scorer 树，返回 top_k 结果。
    // 接受 ISegmentReader 使内存 segment 可复用相同路径。
    std::vector<SearchResult> searchImpl(
        const BooleanQuery& bq,
        int top_k,
        const ISegmentReader& seg,
        const std::unordered_map<std::string, float>& term_idfs,
        const NumericFilter* filter
    ) const;

    [[deprecated("use searchImpl via BooleanQuery")]]
    std::vector<SearchResult> searchAND(
        const std::vector<FieldTerm>& fterms,
        const std::unordered_map<std::string, float>& term_idfs,
        int top_k,
        const SegmentReader& seg,
        const NumericFilter* filter
    ) const;

    [[deprecated("use searchImpl via BooleanQuery")]]
    std::vector<SearchResult> searchOR_WAND(
        const std::vector<FieldTerm>& fterms,
        const std::unordered_map<std::string, float>& term_idfs,
        int top_k,
        const SegmentReader& seg,
        const NumericFilter* filter
    ) const;

    float scoreDoc(DocId doc_id,
                   const std::vector<FieldTerm>& expanded,
                   const std::unordered_map<std::string, float>& term_idfs,
                   const SegmentReader& seg) const;

    bool passesFilter(const ISegmentReader& seg,
                      DocId doc_id,
                      const NumericFilter& filter) const;

    std::vector<SearchResult> mergeTopK(
        std::vector<std::vector<SearchResult>>& per_seg_results,
        int top_k
    ) const;

    void printTermDebug(const SegmentReader& seg,
                        const std::vector<FieldTerm>& fterms,
                        const std::unordered_map<std::string, float>& term_idfs) const;

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

    std::vector<std::string> default_search_fields_;

    // ── 实时 segment（Step 6/7）──────────────────────────────────────────────
    // 通过 shared_ptr 管理生命周期：search() 在搜索开始时获取快照，
    // 保证 arena 在本次搜索期间不被 reset（即使外部调用了 attachRealtime 换了一个新的）。
    mutable std::shared_mutex             rt_mutex_;
    std::shared_ptr<const ISegmentReader> rt_reader_;
};

} // namespace ii
