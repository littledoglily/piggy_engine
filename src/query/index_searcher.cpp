#include "query/index_searcher.h"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// 构造：扫描目录，加载所有 Segment
// ─────────────────────────────────────────────────────────────────────────────

IndexSearcher::IndexSearcher(const std::string& dir) {
    // 扫描目录，找所有 .si 文件（每个对应一个 Segment）
    std::vector<uint32_t> seg_ids;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        auto name = entry.path().filename().string();
        if (name.size() > 4 &&
            name.substr(0, 1) == "_" &&
            name.substr(name.size() - 3) == ".si")
        {
            // 文件名格式：_N.si
            try {
                uint32_t id = std::stoul(name.substr(1, name.size() - 4));
                seg_ids.push_back(id);
            } catch (...) {}
        }
    }
    std::sort(seg_ids.begin(), seg_ids.end());

    for (uint32_t id : seg_ids) {
        segments_.push_back(std::make_unique<SegmentReader>(dir, id));
    }
    std::cout << "[IndexSearcher] Loaded " << segments_.size() << " segment(s).\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// search：主入口
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> IndexSearcher::search(
    const std::string& query,
    int                top_k,
    QueryMode          mode) const
{
    std::vector<std::string> terms = analyzer_.analyzeQuery(query);
    if (terms.empty()) return {};

    std::cout << "[Search] Query terms: [";
    for (size_t i = 0; i < terms.size(); ++i) {
        std::cout << terms[i];
        if (i + 1 < terms.size()) std::cout << ", ";
    }
    std::cout << "] mode=" << (mode == QueryMode::AND ? "AND" : "OR") << "\n";

    std::vector<std::vector<SearchResult>> per_seg;
    for (const auto& seg : segments_) {
        std::vector<SearchResult> seg_results;
        if (mode == QueryMode::AND) {
            seg_results = searchAND(terms, top_k, *seg, nullptr);
        } else {
            seg_results = searchOR_WAND(terms, top_k, *seg, nullptr);
        }
        per_seg.push_back(std::move(seg_results));
    }

    return mergeTopK(per_seg, top_k);
}

// 带数值过滤的重载
std::vector<SearchResult> IndexSearcher::search(
    const std::string&   query,
    const NumericFilter& filter,
    int                  top_k,
    QueryMode            mode) const
{
    std::vector<std::string> terms = analyzer_.analyzeQuery(query);
    if (terms.empty()) return {};

    std::cout << "[Search+Filter] Query terms: [";
    for (size_t i = 0; i < terms.size(); ++i) {
        std::cout << terms[i];
        if (i + 1 < terms.size()) std::cout << ", ";
    }
    std::cout << "] mode=" << (mode == QueryMode::AND ? "AND" : "OR") << "\n";

    std::vector<std::vector<SearchResult>> per_seg;
    for (const auto& seg : segments_) {
        std::vector<SearchResult> seg_results;
        if (mode == QueryMode::AND) {
            seg_results = searchAND(terms, top_k, *seg, &filter);
        } else {
            seg_results = searchOR_WAND(terms, top_k, *seg, &filter);
        }
        per_seg.push_back(std::move(seg_results));
    }

    auto results = mergeTopK(per_seg, top_k);

    // 如果需要按 pubtime 排序（postfilter sort）
    if (filter.sort_by_pubtime) {
        std::stable_sort(results.begin(), results.end(),
            [](const SearchResult& a, const SearchResult& b) {
                return a.pubtime > b.pubtime;
            });
    }
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// passesFilter：检查单个 doc 是否通过数值过滤（in-filter，O(1) 查列存）
// ─────────────────────────────────────────────────────────────────────────────

bool IndexSearcher::passesFilter(const SegmentReader& seg,
                                  DocId doc_id,
                                  const NumericFilter& filter) const
{
    // local_doc_idx = doc_id - 1（doc_id 从 1 开始）
    uint32_t idx = static_cast<uint32_t>(doc_id) - 1;

    if (filter.hasPubtimeRange()) {
        int64_t pt = seg.ffPubtime(idx);
        if (pt < filter.pubtime_lo || pt > filter.pubtime_hi) return false;
    }
    if (filter.hasUidFilter()) {
        if (seg.ffUid(idx) != filter.uid) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// searchAND：Zigzag 双指针求交集
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> IndexSearcher::searchAND(
    const std::vector<std::string>& terms,
    int top_k,
    const SegmentReader& seg,
    const NumericFilter* filter) const
{
    if (terms.empty()) return {};

    for (const auto& t : terms) {
        if (!seg.getTermMeta(t)) return {};
    }

    std::vector<std::vector<DocId>> lists;
    lists.reserve(terms.size());
    for (const auto& t : terms) {
        lists.push_back(seg.readPostingList(t));
        if (lists.back().empty()) return {};
    }

    std::vector<size_t> order(lists.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return lists[a].size() < lists[b].size();
    });

    std::vector<DocId> intersection;
    const auto& driver = lists[order[0]];

    for (DocId candidate : driver) {
        if (!seg.isAlive(candidate)) continue;
        if (filter && !passesFilter(seg, candidate, *filter)) continue;
        bool in_all = true;
        for (size_t i = 1; i < order.size(); ++i) {
            const auto& lst = lists[order[i]];
            auto it = std::lower_bound(lst.begin(), lst.end(), candidate);
            if (it == lst.end() || *it != candidate) {
                in_all = false;
                break;
            }
        }
        if (in_all) intersection.push_back(candidate);
    }

    std::vector<SearchResult> results;
    results.reserve(intersection.size());
    for (DocId did : intersection) {
        float score = seg.bm25Score(did, terms);
        auto stored = seg.readStoredDoc(did);
        SearchResult r;
        r.doc_id  = did;
        r.score   = score;
        r.ext_id  = stored.ext_id;
        r.source  = stored.source;
        r.title   = stored.title;
        r.pubtime = seg.ffPubtime(static_cast<uint32_t>(did) - 1);
        r.uid     = seg.ffUid(static_cast<uint32_t>(did) - 1);
        results.push_back(r);
    }

    std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
        return a.score > b.score;
    });
    if ((int)results.size() > top_k) results.resize(top_k);
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// searchOR_WAND：WAND 算法（OR 语义 TopK）
//
// 流程：
//   1. 为每个 term 维护一个指针（当前 doc_id）
//   2. 按当前 doc_id 升序排列 term 指针
//   3. 从左累加 UB，找到第一个累加 ≥ θ 的 term（pivot）
//   4. 若最小 doc_id == pivot_doc → 精确计算，尝试入堆
//      否则 → 把小于 pivot_doc 的指针跳跃到 pivot_doc
//   5. 更新 θ = 堆中最小分数（堆满后）
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> IndexSearcher::searchOR_WAND(
    const std::vector<std::string>& terms,
    int top_k,
    const SegmentReader& seg,
    const NumericFilter* filter) const
{
    struct TermCursor {
        std::string         term;
        std::vector<DocId>  docs;
        size_t              ptr;
        float               ub;
        DocId curDoc() const {
            return ptr < docs.size() ? docs[ptr] : INVALID_DOC;
        }
    };

    std::vector<TermCursor> cursors;
    for (const auto& t : terms) {
        const TermMeta* meta = seg.getTermMeta(t);
        if (!meta) continue;
        TermCursor c;
        c.term = t;
        c.docs = seg.readPostingList(t);
        c.ptr  = 0;
        c.ub   = meta->upper_bound;
        if (!c.docs.empty()) cursors.push_back(std::move(c));
    }
    if (cursors.empty()) return {};

    MinHeap heap;
    float theta = 0.0f;

    bool changed = true;
    while (changed) {
        changed = false;

        std::sort(cursors.begin(), cursors.end(), [](const TermCursor& a, const TermCursor& b) {
            return a.curDoc() < b.curDoc();
        });

        while (!cursors.empty() && cursors.back().curDoc() == INVALID_DOC)
            cursors.pop_back();
        if (cursors.empty()) break;

        float ub_sum = 0.0f;
        size_t pivot_idx = cursors.size();
        for (size_t i = 0; i < cursors.size(); ++i) {
            ub_sum += cursors[i].ub;
            if (ub_sum >= theta) { pivot_idx = i; break; }
        }
        if (pivot_idx == cursors.size()) break;

        DocId pivot_doc = cursors[pivot_idx].curDoc();
        if (pivot_doc == INVALID_DOC) break;

        DocId min_doc = cursors[0].curDoc();

        if (min_doc == pivot_doc) {
            if (seg.isAlive(pivot_doc) &&
                (!filter || passesFilter(seg, pivot_doc, *filter))) {
                float score = seg.bm25Score(pivot_doc, terms);
                if ((int)heap.size() < top_k || score > heap.top().score) {
                    if ((int)heap.size() == top_k) heap.pop();
                    heap.push({score, pivot_doc});
                    if ((int)heap.size() == top_k) theta = heap.top().score;
                }
            }
            for (auto& c : cursors) {
                if (c.curDoc() == pivot_doc) {
                    while (c.ptr < c.docs.size() && c.docs[c.ptr] <= pivot_doc)
                        ++c.ptr;
                }
            }
            changed = true;
        } else {
            for (auto& c : cursors) {
                if (c.curDoc() < pivot_doc) {
                    auto it = std::lower_bound(c.docs.begin() + c.ptr,
                                               c.docs.end(), pivot_doc);
                    c.ptr = std::distance(c.docs.begin(), it);
                }
            }
            changed = true;
        }
    }

    std::vector<SearchResult> results;
    while (!heap.empty()) {
        auto e = heap.top(); heap.pop();
        auto stored = seg.readStoredDoc(e.doc_id);
        SearchResult r;
        r.doc_id  = e.doc_id;
        r.score   = e.score;
        r.ext_id  = stored.ext_id;
        r.source  = stored.source;
        r.title   = stored.title;
        r.pubtime = seg.ffPubtime(static_cast<uint32_t>(e.doc_id) - 1);
        r.uid     = seg.ffUid(static_cast<uint32_t>(e.doc_id) - 1);
        results.push_back(r);
    }
    std::reverse(results.begin(), results.end());
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// mergeTopK：跨 Segment 归并
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> IndexSearcher::mergeTopK(
    std::vector<std::vector<SearchResult>>& per_seg_results,
    int top_k) const
{
    std::vector<SearchResult> all;
    for (auto& seg_res : per_seg_results) {
        for (auto& r : seg_res) all.push_back(std::move(r));
    }
    std::sort(all.begin(), all.end(), [](const SearchResult& a, const SearchResult& b) {
        return a.score > b.score;
    });
    if ((int)all.size() > top_k) all.resize(top_k);
    return all;
}

// ─────────────────────────────────────────────────────────────────────────────
// printResults
// ─────────────────────────────────────────────────────────────────────────────

void IndexSearcher::printResults(const std::vector<SearchResult>& results) {
    if (results.empty()) {
        std::cout << "  (no results)\n";
        return;
    }
    for (size_t i = 0; i < results.size(); ++i) {
        printf("  #%zu  DocID=%-4u  Score=%.4f  Title=%s\n",
               i + 1, results[i].doc_id, results[i].score,
               results[i].title.c_str());
    }
}

} // namespace ii
