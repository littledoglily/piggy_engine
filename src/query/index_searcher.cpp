#include "query/index_searcher.h"
#include "postings/skiplist.h"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <set>
#include <stdexcept>

// 抑制 deprecated 警告：searchAND / searchOR_WAND 在本文件内部仍被调用
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// 构造：扫描目录，加载所有 Segment；初始化 default_search_fields_
// ─────────────────────────────────────────────────────────────────────────────

IndexSearcher::IndexSearcher(const std::string& dir) {
    std::vector<uint32_t> seg_ids;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        auto name = entry.path().filename().string();
        if (name.size() > 4 && name[0] == '_' &&
            name.substr(name.size() - 3) == ".si")
        {
            try {
                uint32_t id = std::stoul(name.substr(1, name.size() - 4));
                seg_ids.push_back(id);
            } catch (...) {}
        }
    }
    std::sort(seg_ids.begin(), seg_ids.end());

    for (uint32_t id : seg_ids)
        segments_.push_back(std::make_unique<SegmentReader>(dir, id));

    for (const auto& seg : segments_)
        global_total_docs_ += seg->docCount();

    // 默认搜索字段：从第一个 Segment 的 indexedFieldNames() 初始化
    // 用于裸 term 展开（AND 模式：所有字段必须命中；OR 模式：任意字段命中）
    if (!segments_.empty())
        default_search_fields_ = segments_.front()->indexedFieldNames();

    std::cout << "[IndexSearcher] Loaded " << segments_.size()
              << " segment(s), " << global_total_docs_ << " docs total.\n";
    if (!default_search_fields_.empty()) {
        std::cout << "[IndexSearcher] Default search fields: [";
        for (size_t i = 0; i < default_search_fields_.size(); ++i) {
            if (i) std::cout << ", ";
            std::cout << default_search_fields_[i];
        }
        std::cout << "]\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// parseQuery：将原始查询字符串解析为 FieldTerm 列表
//
// 语法：空格分隔；"field:term" 为字段限定；裸词为 bare term（field=""）
// 每个 token 的 term 部分经过 Analyzer 分析（lower-case + stemming）
// "title:python machine" → [{field="title",term="python"}, {field="",term="machin"}]
// ─────────────────────────────────────────────────────────────────────────────

std::vector<FieldTerm> IndexSearcher::parseQuery(const std::string& raw_query) const {
    std::vector<FieldTerm> result;
    std::istringstream iss(raw_query);
    std::string token;

    while (iss >> token) {
        auto colon = token.find(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < token.size()) {
            // field:term 形式
            std::string field    = token.substr(0, colon);
            std::string raw_term = token.substr(colon + 1);
            for (const auto& t : analyzer_.analyzeQuery(raw_term))
                result.push_back({field, t});
        } else {
            // 裸 term
            for (const auto& t : analyzer_.analyzeQuery(token))
                result.push_back({"", t});
        }
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// computeFieldTermIdfs：为所有 (field, term) 对计算 IDF
//
// 结果 key 格式：
//   field-qualified term  → "field:term"
//   bare term（每个默认字段各一条）→ "field:term"
//   bare term global fallback → bare "term"（供 legacy bm25Score 回退）
// ─────────────────────────────────────────────────────────────────────────────

std::unordered_map<std::string, float> IndexSearcher::computeFieldTermIdfs(
    const std::vector<FieldTerm>& fterms) const
{
    if (debug_) {
        printf("\n[IDF] N=%u  (global_total_docs)\n", global_total_docs_);
        printf("  %-20s  %-10s  %s\n", "key", "global_df",
               "IDF = log(1+(N-df+0.5)/(df+0.5))");
        printf("  %s\n", std::string(60, '-').c_str());
    }

    // 收集所有需要计算的 (field, term) 对
    std::set<std::pair<std::string, std::string>> pairs;
    for (const auto& ft : fterms) {
        if (!ft.field.empty()) {
            pairs.insert({ft.field, ft.term});
        } else {
            // bare term：为每个默认字段各计算一次
            for (const auto& f : default_search_fields_)
                pairs.insert({f, ft.term});
            // 若无默认字段（legacy），用全局合并
            if (default_search_fields_.empty())
                pairs.insert({"", ft.term});
        }
    }

    std::unordered_map<std::string, float> result;
    const float N = static_cast<float>(global_total_docs_);

    auto calcIdf = [&](const std::string& key, uint32_t df) {
        if (df == 0) { result[key] = 0.f; return; }
        float idf = std::log(1.f + (N - (float)df + 0.5f) / ((float)df + 0.5f));
        result[key] = idf;
        if (debug_) printf("  %-20s  %-10u  %.4f\n", key.c_str(), df, idf);
    };

    for (const auto& [field, term] : pairs) {
        std::string key = field.empty() ? term : (field + ":" + term);
        if (result.count(key)) continue;

        uint32_t df = 0;
        for (const auto& seg : segments_) {
            if (!field.empty()) {
                const TermMeta* m = seg->getTermMeta(field, term);
                if (m) df += m->doc_freq;
            } else {
                // legacy：用合并词典
                const TermMeta* m = seg->getTermMeta(term);
                if (m) df += m->doc_freq;
            }
        }
        calcIdf(key, df);
    }

    // 也为每个 bare term 存一条全局 key（供 legacy bm25Score 回退查找）
    for (const auto& ft : fterms) {
        if (ft.field.empty() && !result.count(ft.term)) {
            uint32_t df = 0;
            for (const auto& seg : segments_) {
                const TermMeta* m = seg->getTermMeta(ft.term);
                if (m) df += m->doc_freq;
            }
            calcIdf(ft.term, df);
        }
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// scoreDoc：对 doc_id 计算所有展开 FieldTerm 的 per-field BM25 之和
//
// expanded 中 field 不为 "" 时用 per-field bm25Score；
// field 为 ""（legacy）时用 legacy bm25Score（不感知字段）
// ─────────────────────────────────────────────────────────────────────────────

float IndexSearcher::scoreDoc(
    DocId doc_id,
    const std::vector<FieldTerm>& expanded,
    const std::unordered_map<std::string, float>& term_idfs,
    const SegmentReader& seg) const
{
    // 按字段分组，每个字段调用一次 bm25Score
    std::unordered_map<std::string, std::vector<std::string>> f2t;
    for (const auto& ft : expanded) f2t[ft.field].push_back(ft.term);

    float score = 0.f;
    for (const auto& [field, terms] : f2t) {
        if (!field.empty()) {
            score += seg.bm25Score(doc_id, field, terms, term_idfs);
        } else {
            // Legacy path: bare term, use merged dict bm25Score
            std::unordered_map<std::string, float> bare_idfs;
            for (const auto& t : terms) {
                auto it = term_idfs.find(t);
                if (it != term_idfs.end()) bare_idfs[t] = it->second;
            }
            score += seg.bm25Score(doc_id, terms, bare_idfs);
        }
    }
    return score;
}

// ─────────────────────────────────────────────────────────────────────────────
// search：主入口
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> IndexSearcher::search(
    const std::string& query,
    int                top_k,
    QueryMode          mode) const
{
    Occur default_occur = (mode == QueryMode::AND) ? Occur::MUST : Occur::SHOULD;
    QueryParser parser(analyzer_, default_search_fields_);
    auto bq = parser.parse(query, default_occur);

    std::cout << "[Search] Query: " << bq->debugString()
              << " mode=" << (mode == QueryMode::AND ? "AND" : "OR") << "\n";

    auto term_idfs = computeIdfsFromBQ(*bq);

    std::vector<std::vector<SearchResult>> per_seg;
    for (const auto& seg : segments_)
        per_seg.push_back(searchImpl(*bq, top_k, *seg, term_idfs, nullptr));
    return mergeTopK(per_seg, top_k);
}

// 带数值过滤的重载
std::vector<SearchResult> IndexSearcher::search(
    const std::string&   query,
    const NumericFilter& filter,
    int                  top_k,
    QueryMode            mode) const
{
    Occur default_occur = (mode == QueryMode::AND) ? Occur::MUST : Occur::SHOULD;
    QueryParser parser(analyzer_, default_search_fields_);
    auto bq = parser.parse(query, default_occur);

    std::cout << "[Search+Filter] Query: " << bq->debugString()
              << " mode=" << (mode == QueryMode::AND ? "AND" : "OR") << "\n";

    auto term_idfs = computeIdfsFromBQ(*bq);

    std::vector<std::vector<SearchResult>> per_seg;
    for (const auto& seg : segments_)
        per_seg.push_back(searchImpl(*bq, top_k, *seg, term_idfs, &filter));

    auto results = mergeTopK(per_seg, top_k);
    if (filter.sort_by_pubtime) {
        std::stable_sort(results.begin(), results.end(),
            [](const SearchResult& a, const SearchResult& b) {
                return a.pubtime > b.pubtime;
            });
    }
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// passesFilter
// ─────────────────────────────────────────────────────────────────────────────

bool IndexSearcher::passesFilter(const SegmentReader& seg,
                                  DocId doc_id,
                                  const NumericFilter& filter) const
{
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
//
// 裸 term（field=""）在 AND 模式下展开到所有默认字段：
//   每个字段都必须命中，才能进入交集。
// field-qualified term：只查指定字段。
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> IndexSearcher::searchAND(
    const std::vector<FieldTerm>& fterms,
    const std::unordered_map<std::string, float>& term_idfs,
    int top_k,
    const SegmentReader& seg,
    const NumericFilter* filter) const
{
    if (fterms.empty()) return {};

    if (debug_) printTermDebug(seg, fterms, term_idfs);

    // ── 展开 bare terms ──────────────────────────────────────────────────────
    // AND 模式：裸 term 必须在所有默认字段中出现（严格）
    std::vector<FieldTerm> expanded;
    for (const auto& ft : fterms) {
        if (!ft.field.empty()) {
            // 字段限定：只查该字段
            if (!seg.getTermMeta(ft.field, ft.term)) return {};
            expanded.push_back(ft);
        } else if (!default_search_fields_.empty()) {
            // Per-field 模式：必须在所有默认字段中出现
            for (const auto& f : default_search_fields_) {
                if (!seg.getTermMeta(f, ft.term)) return {};
                expanded.push_back({f, ft.term});
            }
        } else {
            // Legacy 模式：用合并词典检查
            if (!seg.getTermMeta(ft.term)) return {};
            expanded.push_back(ft);  // field = ""
        }
    }
    if (expanded.empty()) return {};

    // ── 按 df 升序排列（最短 posting list 驱动 Zigzag）─────────────────────
    std::vector<size_t> order(expanded.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        auto get_df = [&](const FieldTerm& ft) -> uint32_t {
            if (!ft.field.empty()) {
                const TermMeta* m = seg.getTermMeta(ft.field, ft.term);
                return m ? m->doc_freq : 0;
            }
            const TermMeta* m = seg.getTermMeta(ft.term);
            return m ? m->doc_freq : 0;
        };
        return get_df(expanded[a]) < get_df(expanded[b]);
    });

    // ── 创建惰性迭代器（每个独立文件句柄）──────────────────────────────────
    std::vector<PostingIterator> iters;
    iters.reserve(expanded.size());
    for (size_t idx : order) {
        const auto& ft = expanded[idx];
        PostingIterator it = ft.field.empty()
            ? seg.postingIterator(ft.term)             // legacy
            : seg.postingIterator(ft.field, ft.term);  // per-field
        if (it.isEnd()) return {};
        iters.push_back(std::move(it));
    }

    // ── Zigzag AND：driver = iters[0]（df 最小），其余追赶 ─────────────────
    std::vector<DocId> intersection;
    bool done = false;
    while (!done && !iters[0].isEnd()) {
        DocId target  = iters[0].docId();
        bool  matched = true;
        for (size_t i = 1; i < iters.size(); ++i) {
            if (!iters[i].advance(target)) { done = true; break; }
            if (iters[i].docId() != target) {
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

    // ── 打分 ─────────────────────────────────────────────────────────────────
    std::vector<SearchResult> results;
    results.reserve(intersection.size());
    for (DocId did : intersection) {
        float score = scoreDoc(did, expanded, term_idfs, seg);
        auto stored = seg.readStoredDoc(did);
        SearchResult r;
        r.doc_id        = did;
        r.score         = score;
        r.ext_id        = stored.ext_id;
        r.source        = stored.source();
        r.title         = stored.title();
        r.stored_fields = stored.str_fields;
        r.pubtime       = seg.ffPubtime(static_cast<uint32_t>(did) - 1);
        r.uid           = seg.ffUid(static_cast<uint32_t>(did) - 1);
        results.push_back(r);
    }
    std::sort(results.begin(), results.end(),
        [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
    if ((int)results.size() > top_k) results.resize(top_k);
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// searchOR_WAND：WAND 算法（OR 语义 TopK）
//
// 裸 term（field=""）在 OR 模式下展开到所有默认字段，每个字段对应一个 cursor；
// field-qualified term：一个 cursor。
//
// TermCursor 携带 field 信息，方便 per-field BM25 打分。
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> IndexSearcher::searchOR_WAND(
    const std::vector<FieldTerm>& fterms,
    const std::unordered_map<std::string, float>& term_idfs,
    int top_k,
    const SegmentReader& seg,
    const NumericFilter* filter) const
{
    // ── TermCursor（per-field 版）────────────────────────────────────────────
    struct TermCursor {
        std::string     field;      // "" = legacy
        std::string     term;
        PostingIterator iter;
        float           idf;
        float           list_ub;   // 静态：全局 max_tf_norm × IDF（WAND pivot 判断）
        float           block_ub;  // 动态：当前 Block 的 UB（BlockMaxWAND 细筛）
        DocId curDoc() const { return iter.docId(); }
        void refreshBlockUb() { block_ub = iter.blockMaxScore() * idf; }
    };

    if (debug_) printTermDebug(seg, fterms, term_idfs);

    // ── 构建 cursors（展开 bare terms 到所有默认字段）────────────────────────
    std::vector<TermCursor> cursors;
    auto addCursor = [&](const std::string& field, const std::string& term) {
        std::string idf_key = field.empty() ? term : (field + ":" + term);
        auto idf_it = term_idfs.find(idf_key);
        float idf = idf_it != term_idfs.end() ? idf_it->second : 0.f;

        const TermMeta* meta = field.empty()
            ? seg.getTermMeta(term)             // legacy
            : seg.getTermMeta(field, term);     // per-field
        if (!meta) return;

        TermCursor c;
        c.field    = field;
        c.term     = term;
        c.idf      = idf;
        c.list_ub  = meta->upper_bound * idf;
        c.iter     = field.empty()
            ? seg.postingIterator(term)          // legacy
            : seg.postingIterator(field, term);  // per-field
        c.block_ub = c.iter.blockMaxScore() * idf;
        if (!c.iter.isEnd()) cursors.push_back(std::move(c));
    };

    for (const auto& ft : fterms) {
        if (!ft.field.empty()) {
            addCursor(ft.field, ft.term);
        } else if (!default_search_fields_.empty()) {
            // OR 模式：裸 term 展开到所有默认字段，任意命中即可
            for (const auto& f : default_search_fields_)
                addCursor(f, ft.term);
        } else {
            // Legacy 模式
            addCursor("", ft.term);
        }
    }
    if (cursors.empty()) return {};

    // ── WAND 主循环 ──────────────────────────────────────────────────────────
    MinHeap heap;
    float   theta  = 0.f;
    bool    changed = true;

    // 预先收集所有 (field, term) 对，供 pivot_doc 打分用
    // 按字段分组：field → unique terms（去重）
    std::unordered_map<std::string, std::vector<std::string>> score_f2t;
    {
        std::unordered_map<std::string, std::set<std::string>> tmp;
        for (const auto& c : cursors) tmp[c.field].insert(c.term);
        for (auto& [f, ts] : tmp)
            score_f2t[f].assign(ts.begin(), ts.end());
    }

    while (changed) {
        changed = false;

        std::sort(cursors.begin(), cursors.end(),
            [](const TermCursor& a, const TermCursor& b) {
                return a.curDoc() < b.curDoc();
            });

        while (!cursors.empty() && cursors.back().curDoc() == INVALID_DOC)
            cursors.pop_back();
        if (cursors.empty()) break;

        // ── WAND pivot 选择（list_ub 保守上界，保证不漏召回）────────────────
        float  ub_sum    = 0.f;
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
            // ── BlockMaxWAND 细筛 ─────────────────────────────────────────────
            float block_ub_sum = 0.f;
            for (const auto& c : cursors) block_ub_sum += c.block_ub;

            if (block_ub_sum < theta) {
                // Block 级跳跃：所有 cursor 跳过当前 Block
                for (auto& c : cursors) {
                    DocId blk_end = c.iter.blockMaxDocId();
                    if (blk_end != INVALID_DOC) c.iter.advance(blk_end + 1);
                    c.refreshBlockUb();
                }
            } else {
                // 精确打分：对 pivot_doc 计算 per-field BM25
                if (seg.isAlive(pivot_doc) &&
                    (!filter || passesFilter(seg, pivot_doc, *filter)))
                {
                    float score = 0.f;
                    for (const auto& [field, terms] : score_f2t) {
                        if (!field.empty()) {
                            score += seg.bm25Score(pivot_doc, field, terms, term_idfs);
                        } else {
                            // Legacy
                            std::unordered_map<std::string, float> bare_idfs;
                            for (const auto& t : terms) {
                                auto it = term_idfs.find(t);
                                if (it != term_idfs.end()) bare_idfs[t] = it->second;
                            }
                            score += seg.bm25Score(pivot_doc, terms, bare_idfs);
                        }
                    }
                    if ((int)heap.size() < top_k || score > heap.top().score) {
                        if ((int)heap.size() == top_k) heap.pop();
                        heap.push({score, pivot_doc});
                        if ((int)heap.size() == top_k) theta = heap.top().score;
                    }
                }
                // 推进处于 pivot_doc 的 cursor
                for (auto& c : cursors) {
                    if (c.curDoc() == pivot_doc) {
                        c.iter.advance(pivot_doc + 1);
                        c.refreshBlockUb();
                    }
                }
            }
            changed = true;
        } else {
            // 落后于 pivot_doc 的 cursor 跳跃到 pivot_doc
            for (auto& c : cursors) {
                if (c.curDoc() < pivot_doc) {
                    c.iter.advance(pivot_doc);
                    c.refreshBlockUb();
                }
            }
            changed = true;
        }
    }

    std::vector<SearchResult> results;
    while (!heap.empty()) {
        auto e      = heap.top(); heap.pop();
        auto stored = seg.readStoredDoc(e.doc_id);
        SearchResult r;
        r.doc_id        = e.doc_id;
        r.score         = e.score;
        r.ext_id        = stored.ext_id;
        r.source        = stored.source();
        r.title         = stored.title();
        r.stored_fields = stored.str_fields;
        r.pubtime       = seg.ffPubtime(static_cast<uint32_t>(e.doc_id) - 1);
        r.uid           = seg.ffUid(static_cast<uint32_t>(e.doc_id) - 1);
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
    for (auto& seg_res : per_seg_results)
        for (auto& r : seg_res) all.push_back(std::move(r));

    std::sort(all.begin(), all.end(),
        [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
    if ((int)all.size() > top_k) all.resize(top_k);
    return all;
}

// ─────────────────────────────────────────────────────────────────────────────
// printTermDebug：打印 term/field 调试信息（--debug 模式）
// ─────────────────────────────────────────────────────────────────────────────

void IndexSearcher::printTermDebug(
    const SegmentReader& seg,
    const std::vector<FieldTerm>& fterms,
    const std::unordered_map<std::string, float>& term_idfs) const
{
    printf("\n[TermDebug] seg=%u\n", seg.segmentId());
    printf("  %-22s  %-6s  %-5s  %s\n", "field:term", "df", "nodes", "UB");
    printf("  %s\n", std::string(70, '-').c_str());

    // Collect unique (field, term) pairs to debug
    std::set<std::pair<std::string, std::string>> printed;
    auto debugOne = [&](const std::string& field, const std::string& term) {
        if (!printed.insert({field, term}).second) return;
        std::string display = field.empty() ? term : field + ":" + term;
        std::string idf_key = field.empty() ? term : field + ":" + term;

        const TermMeta* meta = field.empty()
            ? seg.getTermMeta(term)
            : seg.getTermMeta(field, term);

        if (!meta) {
            printf("  \"%-20s\"  (not in seg%u)\n", display.c_str(), seg.segmentId());
            return;
        }
        auto idf_it = term_idfs.find(idf_key);
        float idf   = idf_it != term_idfs.end() ? idf_it->second : 0.f;
        float eff_ub = meta->upper_bound * idf;
        printf("  \"%-20s\"  df=%-5u  ub=%.4f×%.4f=%.4f\n",
               display.c_str(), meta->doc_freq, meta->upper_bound, idf, eff_ub);
    };

    for (const auto& ft : fterms) {
        if (!ft.field.empty()) {
            debugOne(ft.field, ft.term);
        } else {
            for (const auto& f : default_search_fields_)
                debugOne(f, ft.term);
            if (default_search_fields_.empty())
                debugOne("", ft.term);
        }
    }
    printf("\n");
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

// ─────────────────────────────────────────────────────────────────────────────
// computeIdfsFromBQ：递归收集 BooleanQuery 树中的 (field,term)，计算 IDF
//
// 与 computeFieldTermIdfs 使用相同公式：
//   IDF = log(1 + (N - df + 0.5) / (df + 0.5))
// ─────────────────────────────────────────────────────────────────────────────

std::unordered_map<std::string, float> IndexSearcher::computeIdfsFromBQ(
    const BooleanQuery& bq) const
{
    std::vector<std::pair<std::string,std::string>> raw;
    bq.collectTerms(raw);

    // 去重
    std::set<std::pair<std::string,std::string>> unique(raw.begin(), raw.end());

    std::unordered_map<std::string, float> result;
    const float N = static_cast<float>(global_total_docs_);

    for (const auto& [field, term] : unique) {
        std::string key = field.empty() ? term : (field + ":" + term);
        if (result.count(key)) continue;

        uint32_t df = 0;
        for (const auto& seg : segments_) {
            const TermMeta* m = field.empty()
                ? seg->getTermMeta(term)
                : seg->getTermMeta(field, term);
            if (m) df += m->doc_freq;
        }

        if (df == 0) { result[key] = 0.f; continue; }
        result[key] = std::log(1.f + (N - (float)df + 0.5f) / ((float)df + 0.5f));
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// searchImpl：在单个 Segment 上驱动 Scorer 树，返回 top_k 结果
//
// 根节点类型路由：
//   WANDScorer          → collectTopK()（内部已做 top_k 截断）
//   其他（AND/Exclusion）→ 顺序驱动，打分后排序截断
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> IndexSearcher::searchImpl(
    const BooleanQuery& bq,
    int top_k,
    const SegmentReader& seg,
    const std::unordered_map<std::string, float>& term_idfs,
    const NumericFilter* filter) const
{
    ScorerContext ctx{seg, term_idfs, global_total_docs_, filter, top_k};
    auto root = bq.createScorer(ctx);
    if (!root) return {};

    // WANDScorer（OR TopK）：构造时已跑完 WAND，直接读缓存结果
    if (auto* wand = dynamic_cast<WANDScorer*>(root.get())) {
        return wand->collectTopK();
    }

    // AND / Exclusion 路径：顺序驱动
    std::vector<SearchResult> results;
    while (!root->isEnd()) {
        DocId did = root->docId();
        if (seg.isAlive(did) && (!filter || passesFilter(seg, did, *filter))) {
            float score = root->score();
            auto stored = seg.readStoredDoc(did);
            SearchResult r;
            r.doc_id        = did;
            r.score         = score;
            r.ext_id        = stored.ext_id;
            r.source        = stored.source();
            r.title         = stored.title();
            r.stored_fields = stored.str_fields;
            r.pubtime       = seg.ffPubtime(static_cast<uint32_t>(did) - 1);
            r.uid           = seg.ffUid(static_cast<uint32_t>(did) - 1);
            results.push_back(std::move(r));
        }
        root->next();
    }

    std::sort(results.begin(), results.end(),
        [](const SearchResult& a, const SearchResult& b) { return a.score > b.score; });
    if ((int)results.size() > top_k) results.resize(top_k);
    return results;
}

#pragma clang diagnostic pop

} // namespace ii
