#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query/index_searcher.h  —  检索入口
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "analysis/analyzer.h"
#include "index/segment_reader.h"
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

    std::unordered_map<std::string, float> computeFieldTermIdfs(
        const std::vector<FieldTerm>& fterms
    ) const;

    std::unordered_map<std::string, float> computeIdfsFromBQ(
        const BooleanQuery& bq) const;

    std::vector<SearchResult> searchImpl(
        const BooleanQuery& bq,
        int top_k,
        const SegmentReader& seg,
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

    bool passesFilter(const SegmentReader& seg,
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
};

} // namespace ii
