#include "index_searcher.h"
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
    // 分析查询词
    std::vector<std::string> terms = analyzer_.analyzeQuery(query);
    if (terms.empty()) return {};

    std::cout << "[Search] Query terms: [";
    for (size_t i = 0; i < terms.size(); ++i) {
        std::cout << terms[i];
        if (i + 1 < terms.size()) std::cout << ", ";
    }
    std::cout << "] mode=" << (mode == QueryMode::AND ? "AND" : "OR") << "\n";

    // 对每个 Segment 独立搜索
    std::vector<std::vector<SearchResult>> per_seg;
    for (const auto& seg : segments_) {
        std::vector<SearchResult> seg_results;
        if (mode == QueryMode::AND) {
            seg_results = searchAND(terms, top_k, *seg);
        } else {
            seg_results = searchOR_WAND(terms, top_k, *seg);
        }
        per_seg.push_back(std::move(seg_results));
    }

    return mergeTopK(per_seg, top_k);
}

// ─────────────────────────────────────────────────────────────────────────────
// searchAND：Zigzag 双指针求交集
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> IndexSearcher::searchAND(
    const std::vector<std::string>& terms,
    int top_k,
    const SegmentReader& seg) const
{
    if (terms.empty()) return {};

    // 检查所有 term 是否在该 Segment 中存在
    for (const auto& t : terms) {
        if (!seg.getTermMeta(t)) return {};
    }

    // 读取所有 term 的 posting list（升序 doc_id）
    std::vector<std::vector<DocId>> lists;
    lists.reserve(terms.size());
    for (const auto& t : terms) {
        lists.push_back(seg.readPostingList(t));
        if (lists.back().empty()) return {};
    }

    // 按 posting list 长度升序排列（最短的作为驱动）
    std::vector<size_t> order(lists.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return lists[a].size() < lists[b].size();
    });

    // Zigzag 交集：以最短 list 为驱动
    std::vector<DocId> intersection;
    const auto& driver = lists[order[0]];

    for (DocId candidate : driver) {
        if (!seg.isAlive(candidate)) continue;
        bool in_all = true;
        for (size_t i = 1; i < order.size(); ++i) {
            const auto& lst = lists[order[i]];
            // 二分查找 candidate 是否在 lst 中
            auto it = std::lower_bound(lst.begin(), lst.end(), candidate);
            if (it == lst.end() || *it != candidate) {
                in_all = false;
                break;
            }
        }
        if (in_all) intersection.push_back(candidate);
    }

    // 对交集中每个 doc 计算 BM25
    std::vector<SearchResult> results;
    results.reserve(intersection.size());
    for (DocId did : intersection) {
        float score = seg.bm25Score(did, terms);
        auto stored = seg.readStoredDoc(did);
        SearchResult r;
        r.doc_id = did;
        r.score  = score;
        r.title  = stored.title;
        results.push_back(r);
    }

    // 按分数排序，取 top_k
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
    const SegmentReader& seg) const
{
    // 过滤在该 Segment 中不存在的 term
    struct TermCursor {
        std::string         term;
        std::vector<DocId>  docs;
        size_t              ptr;   // 当前位置
        float               ub;    // Upper Bound
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

    // MinHeap（分数最小的在堆顶）
    MinHeap heap;  // 保存 TopK 候选（最小堆）
    float theta = 0.0f;  // 堆中最小分数

    // WAND 主循环
    bool changed = true;
    while (changed) {
        changed = false;

        // Step1：按当前 doc_id 升序排序 cursors
        std::sort(cursors.begin(), cursors.end(), [](const TermCursor& a, const TermCursor& b) {
            return a.curDoc() < b.curDoc();
        });

        // 去掉已到末尾的 cursor
        while (!cursors.empty() && cursors.back().curDoc() == INVALID_DOC)
            cursors.pop_back();
        if (cursors.empty()) break;

        // Step2：找 pivot（从左累加 UB >= theta 的第一个 term）
        float ub_sum = 0.0f;
        size_t pivot_idx = cursors.size();  // 默认找不到
        for (size_t i = 0; i < cursors.size(); ++i) {
            ub_sum += cursors[i].ub;
            if (ub_sum >= theta) {
                pivot_idx = i;
                break;
            }
        }
        if (pivot_idx == cursors.size()) break;  // 所有 UB 之和 < theta，终止

        DocId pivot_doc = cursors[pivot_idx].curDoc();
        if (pivot_doc == INVALID_DOC) break;

        // Step3：判断最小 doc_id 是否等于 pivot_doc
        DocId min_doc = cursors[0].curDoc();

        if (min_doc == pivot_doc) {
            // 精确计算 pivot_doc 的分数
            if (seg.isAlive(pivot_doc)) {
                float score = seg.bm25Score(pivot_doc, terms);

                if ((int)heap.size() < top_k || score > heap.top().score) {
                    if ((int)heap.size() == top_k) heap.pop();
                    heap.push({score, pivot_doc});
                    if ((int)heap.size() == top_k) {
                        theta = heap.top().score;
                    }
                }
            }
            // 推进所有指向 pivot_doc 的 cursor
            for (auto& c : cursors) {
                if (c.curDoc() == pivot_doc) {
                    // 跳到下一个 > pivot_doc 的 doc
                    while (c.ptr < c.docs.size() && c.docs[c.ptr] <= pivot_doc)
                        ++c.ptr;
                }
            }
            changed = true;
        } else {
            // min_doc < pivot_doc：把所有 < pivot_doc 的 cursor 跳到 pivot_doc
            for (auto& c : cursors) {
                if (c.curDoc() < pivot_doc) {
                    // 二分跳跃到 >= pivot_doc 的位置
                    auto it = std::lower_bound(c.docs.begin() + c.ptr,
                                               c.docs.end(), pivot_doc);
                    c.ptr = std::distance(c.docs.begin(), it);
                }
            }
            changed = true;
        }
    }

    // 从 heap 取出结果（从小到大，需要反转）
    std::vector<SearchResult> results;
    while (!heap.empty()) {
        auto e = heap.top(); heap.pop();
        auto stored = seg.readStoredDoc(e.doc_id);
        SearchResult r;
        r.doc_id = e.doc_id;
        r.score  = e.score;
        r.title  = stored.title;
        results.push_back(r);
    }
    std::reverse(results.begin(), results.end());  // 高分在前
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
