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
#include <unordered_set>
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

    // ── 多 reader 管理接口 ────────────────────────────────────────────────────
    //
    // addReader：将 reader（active RT / frozen memory / 新磁盘）加入 all_readers_。
    // commitDiskSegment：原子操作——先加入 new_disk，再移除 frozen_to_remove，
    //   保证 doc 在整个 flush 周期内始终可见。
    // removeReader：从 all_readers_ 移除（RT 线程 shutdown 时调用）。
    void addReader(std::shared_ptr<ISegmentReader> reader);
    void commitDiskSegment(std::shared_ptr<ISegmentReader>              new_disk,
                           std::vector<std::shared_ptr<ISegmentReader>> frozen_to_remove);
    void removeReader(std::shared_ptr<ISegmentReader> reader);

    // ── 兼容接口（已有测试使用，内部转发到 addReader/removeReader）────────────
    void attachRealtime(std::shared_ptr<ISegmentReader> rt);
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

    // readers_snap：search() 开始时在 shared_lock 下复制的 all_readers_ 快照。
    // N = 快照中所有 reader 的 docCount() 之和。
    std::unordered_map<std::string, float> computeIdfsFromBQ(
        const BooleanQuery&                                    bq,
        const std::vector<std::shared_ptr<ISegmentReader>>&    readers_snap,
        uint32_t                                               total_docs
    ) const;

    // deprecated path のみ使用（searchAND / searchOR_WAND）
    std::unordered_map<std::string, float> computeFieldTermIdfs(
        const std::vector<FieldTerm>& fterms
    ) const;

    // 在单个 Segment 上驱动 Scorer 树，返回 top_k 结果。
    // total_docs：跨所有 reader 的文档总数，用于 ScorerContext。
    std::vector<SearchResult> searchImpl(
        const BooleanQuery&                            bq,
        int                                            top_k,
        const ISegmentReader&                          seg,
        const std::unordered_map<std::string, float>&  term_idfs,
        const NumericFilter*                           filter,
        uint32_t                                       total_docs
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

    // mergeTopK：跨 segment 归并后按 ext_id 去重（保留最高分），再截断到 top_k。
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
    // 磁盘 segment（供 deprecated searchAND/searchOR_WAND 路径直接访问 SegmentReader&）
    std::vector<std::shared_ptr<SegmentReader>> segments_;
    uint32_t    global_total_docs_ = 0;  // 磁盘 segment 总文档数（deprecated 路径用）
    mutable bool debug_ = false;

    std::vector<std::string> default_search_fields_;

    // ── 统一 reader 列表（disk + frozen memory + active memory）─────────────
    // search() 在 shared_lock 下复制快照，锁外完成所有搜索，保证并发安全。
    mutable std::shared_mutex                       readers_mutex_;
    std::vector<std::shared_ptr<ISegmentReader>>    all_readers_;

    // attachRealtime/detachRealtime 兼容：追踪通过兼容接口注册的单个 RT reader
    std::shared_ptr<ISegmentReader>                 compat_rt_reader_;
};

} // namespace ii
