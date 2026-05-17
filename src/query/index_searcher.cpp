#include "query/index_searcher.h"
#include "postings/skiplist.h"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdio>
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

    for (const auto& seg : segments_) {
        global_total_docs_ += seg->docCount();
    }

    std::cout << "[IndexSearcher] Loaded " << segments_.size()
              << " segment(s), " << global_total_docs_ << " docs total.\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// computeTermIdfs：基于全局统计计算每个 query term 的 IDF
// global_df = 各 segment 中 df 之和；global_idf 用标准 BM25 公式
// ─────────────────────────────────────────────────────────────────────────────

std::unordered_map<std::string, float> IndexSearcher::computeTermIdfs(
    const std::vector<std::string>& terms) const
{
    if (debug_) {
        printf("\n[IDF] N=%u  (global_total_docs)\n", global_total_docs_);
        printf("  %-14s  %-28s  %-12s  %s\n",
               "term", "per-seg df", "global_df", "IDF = log(1+(N-df+0.5)/(df+0.5))");
        printf("  %s\n", std::string(80, '-').c_str());
    }

    std::unordered_map<std::string, float> result;
    for (const auto& term : terms) {
        uint32_t global_df = 0;
        std::string seg_breakdown;

        for (const auto& seg : segments_) {
            const TermMeta* m = seg->getTermMeta(term);
            uint32_t df = m ? m->doc_freq : 0;
            global_df += df;
            if (debug_) {
                if (!seg_breakdown.empty()) seg_breakdown += "  ";
                seg_breakdown += "seg" + std::to_string(seg->segmentId())
                               + ":df=" + std::to_string(df);
            }
        }

        if (global_df == 0) {
            result[term] = 0.0f;
            if (debug_)
                printf("  \"%-12s\"  %-28s  %-12u  (not found, IDF=0)\n",
                       term.c_str(), seg_breakdown.c_str(), global_df);
            continue;
        }

        float N   = static_cast<float>(global_total_docs_);
        float df  = static_cast<float>(global_df);
        float idf = std::log(1.0f + (N - df + 0.5f) / (df + 0.5f));
        result[term] = idf;

        if (debug_)
            printf("  \"%-12s\"  %-28s  %-12u  log(1+(%.1f-%.1f+0.5)/(%.1f+0.5)) = %.4f\n",
                   term.c_str(), seg_breakdown.c_str(),
                   global_df, N, df, df, idf);
    }
    return result;
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

    auto term_idfs = computeTermIdfs(terms);

    std::vector<std::vector<SearchResult>> per_seg;
    for (const auto& seg : segments_) {
        std::vector<SearchResult> seg_results;
        if (mode == QueryMode::AND) {
            seg_results = searchAND(terms, term_idfs, top_k, *seg, nullptr);
        } else {
            seg_results = searchOR_WAND(terms, term_idfs, top_k, *seg, nullptr);
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

    auto term_idfs = computeTermIdfs(terms);

    std::vector<std::vector<SearchResult>> per_seg;
    for (const auto& seg : segments_) {
        std::vector<SearchResult> seg_results;
        if (mode == QueryMode::AND) {
            seg_results = searchAND(terms, term_idfs, top_k, *seg, &filter);
        } else {
            seg_results = searchOR_WAND(terms, term_idfs, top_k, *seg, &filter);
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
    const std::unordered_map<std::string, float>& term_idfs,
    int top_k,
    const SegmentReader& seg,
    const NumericFilter* filter) const
{
    if (terms.empty()) return {};

    for (const auto& t : terms) {
        if (!seg.getTermMeta(t)) return {};
    }

    if (debug_) printTermDebug(seg, terms, term_idfs);

    // 按 df 升序排列，最短列表驱动 Zigzag
    std::vector<size_t> order(terms.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        auto ma = seg.getTermMeta(terms[a]);
        auto mb = seg.getTermMeta(terms[b]);
        return (ma ? ma->doc_freq : 0) < (mb ? mb->doc_freq : 0);
    });

    // 创建惰性迭代器（每个独立文件句柄）
    std::vector<PostingIterator> iters;
    iters.reserve(terms.size());
    for (size_t idx : order) {
        auto it = seg.postingIterator(terms[idx]);
        if (it.isEnd()) return {};
        iters.push_back(std::move(it));
    }

    // Zigzag AND：driver = iters[0]（df 最小），其余追赶
    std::vector<DocId> intersection;
    bool done = false;
    while (!done && !iters[0].isEnd()) {
        DocId target = iters[0].docId();
        bool matched = true;

        for (size_t i = 1; i < iters.size(); ++i) {
            if (!iters[i].advance(target)) { done = true; break; }
            if (iters[i].docId() != target) {
                // 当前 cursor 跳过了 target，driver 追赶到 cursor 位置
                if (!iters[0].advance(iters[i].docId())) { done = true; break; }
                matched = false;
                break;
            }
        }

        if (!done && matched) {
            DocId did = target;
            if (seg.isAlive(did) && (!filter || passesFilter(seg, did, *filter)))
                intersection.push_back(did);
            iters[0].advance(did + 1);
        }
    }

    std::vector<SearchResult> results;
    results.reserve(intersection.size());
    for (DocId did : intersection) {
        float score = seg.bm25Score(did, terms, term_idfs);
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
    const std::unordered_map<std::string, float>& term_idfs,
    int top_k,
    const SegmentReader& seg,
    const NumericFilter* filter) const
{
    // ── TermCursor ────────────────────────────────────────────────────────────
    // list_ub  : 整条 posting list 的上界（WAND pivot 判断，静态不变）
    // block_ub : 当前 Block 的上界（BlockMaxWAND 细筛，每次 advance 后刷新）
    struct TermCursor {
        std::string     term;
        PostingIterator iter;
        float           idf;
        float           list_ub;
        float           block_ub;
        DocId curDoc() const { return iter.docId(); }
        void refreshBlockUb() { block_ub = iter.blockMaxScore() * idf; }
    };

    if (debug_) printTermDebug(seg, terms, term_idfs);

    std::vector<TermCursor> cursors;
    for (const auto& t : terms) {
        const TermMeta* meta = seg.getTermMeta(t);
        if (!meta) continue;
        auto idf_it = term_idfs.find(t);
        float idf = (idf_it != term_idfs.end()) ? idf_it->second : 0.0f;
        TermCursor c;
        c.term     = t;
        c.idf      = idf;
        c.list_ub  = meta->upper_bound * idf;  // 全局 max_tf_norm × IDF，静态
        c.iter     = seg.postingIterator(t);
        c.block_ub = c.iter.blockMaxScore() * idf;  // 第一个 Block 的 UB
        if (!c.iter.isEnd()) cursors.push_back(std::move(c));
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

        // ── WAND pivot 选择：用 list_ub（保守上界）保证不漏召回 ──────────────
        float ub_sum = 0.0f;
        size_t pivot_idx = cursors.size();
        for (size_t i = 0; i < cursors.size(); ++i) {
            ub_sum += cursors[i].list_ub;
            if (ub_sum >= theta) { pivot_idx = i; break; }
        }
        if (pivot_idx == cursors.size()) break;

        DocId pivot_doc = cursors[pivot_idx].curDoc();
        if (pivot_doc == INVALID_DOC) break;

        DocId min_doc = cursors[0].curDoc();

        if (min_doc == pivot_doc) {
            // ── BlockMaxWAND：用所有 cursor 的 block_ub 之和做细筛 ────────────
            // Σ(ALL block_ub) 是任意 doc 得分的保守上界：
            //   · curDoc()==pivot_doc 的 cursor：block_ub 是该 Block 内的最高贡献
            //   · curDoc() > pivot_doc 的 cursor：已越过 pivot_doc，贡献=0 ≤ block_ub
            // 若该上界 < θ，当前所有 Block 内没有任何 doc 能进 TopK
            float block_ub_sum = 0.0f;
            for (const auto& c : cursors) block_ub_sum += c.block_ub;

            if (block_ub_sum < theta) {
                // Block 级跳跃：所有 cursor 跳过当前 Block
                for (auto& c : cursors) {
                    DocId blk_end = c.iter.blockMaxDocId();
                    if (blk_end != INVALID_DOC)
                        c.iter.advance(blk_end + 1);
                    c.refreshBlockUb();
                }
            } else {
                // Block 通过细筛，对 pivot_doc 精确打分
                if (seg.isAlive(pivot_doc) &&
                    (!filter || passesFilter(seg, pivot_doc, *filter))) {
                    float score = seg.bm25Score(pivot_doc, terms, term_idfs);
                    if ((int)heap.size() < top_k || score > heap.top().score) {
                        if ((int)heap.size() == top_k) heap.pop();
                        heap.push({score, pivot_doc});
                        if ((int)heap.size() == top_k) theta = heap.top().score;
                    }
                }
                // 推进处于 pivot_doc 的 cursor 到下一个 doc
                for (auto& c : cursors) {
                    if (c.curDoc() == pivot_doc) {
                        c.iter.advance(pivot_doc + 1);
                        c.refreshBlockUb();
                    }
                }
            }
            changed = true;
        } else {
            // branch-2：落后于 pivot_doc 的 cursor 跳跃到 pivot_doc
            for (auto& c : cursors) {
                if (c.curDoc() < pivot_doc) {
                    c.iter.advance(pivot_doc);
                    c.refreshBlockUb();  // 跨 Block 后刷新 block_ub
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
// printTermDebug：打印每个 term 的 SkipNode 列表 + UB 验证（--debug 模式）
//
// 输出三类信息：
//   1. SkipNode 列表：每个 Block 的 max_doc/max_score/byte_offset/doc_count
//   2. UB 验证：stored max_tf_norm × online IDF = effective UB（WAND 剪枝阈值）
// ─────────────────────────────────────────────────────────────────────────────

void IndexSearcher::printTermDebug(
    const SegmentReader& seg,
    const std::vector<std::string>& terms,
    const std::unordered_map<std::string, float>& term_idfs) const
{
    printf("\n[TermDebug] seg=%u\n", seg.segmentId());
    printf("  %-14s  %-6s  %-5s  %s\n", "term", "df", "nodes", "SkipNode list  +  UB verify");
    printf("  %s\n", std::string(78, '-').c_str());

    for (const auto& t : terms) {
        const TermMeta* meta = seg.getTermMeta(t);
        if (!meta) {
            printf("  \"%-12s\"  (not in seg%u)\n", t.c_str(), seg.segmentId());
            continue;
        }

        SkipList sl = seg.readTermSkipList(t);
        printf("  \"%-12s\"  df=%-5u  nodes=%zu\n",
               t.c_str(), meta->doc_freq, sl.size());

        if (sl.empty()) {
            printf("    (single block — df <= 128, no skip nodes needed)\n");
        } else {
            printf("    %-6s  %-8s  %-10s  %-10s  %s\n",
                   "idx", "max_doc", "max_score", "byte_off", "docs");
            for (size_t i = 0; i < sl.size(); ++i) {
                const SkipNode& n = sl.node(i);
                printf("    [%4zu]  %-8u  %-10.4f  %-10llu  %u\n",
                       i, n.max_doc_id, n.max_score,
                       (unsigned long long)n.byte_offset, n.doc_count);
            }
        }

        auto idf_it = term_idfs.find(t);
        float idf     = (idf_it != term_idfs.end()) ? idf_it->second : 0.0f;
        float eff_ub  = meta->upper_bound * idf;
        // upper_bound 在 flush 时存的是 max_tf_norm（不含 IDF），查询期乘以 global IDF 才是真正 UB
        printf("  [UB verify] max_tf_norm(stored)=%.4f × IDF=%.4f → effective_UB=%.4f\n\n",
               meta->upper_bound, idf, eff_ub);
    }
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
