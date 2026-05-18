#include "query/wand_scorer.h"
#include "segment/segment_reader.h"
#include <algorithm>

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// 构造：缓存全局上界，清除已耗尽子节点，立即运行 WAND 主循环
// ─────────────────────────────────────────────────────────────────────────────

WANDScorer::WANDScorer(std::vector<std::unique_ptr<Scorer>> children,
                       int                                  top_k,
                       const ScorerContext&                 ctx)
    : children_(std::move(children)), top_k_(top_k), ctx_(&ctx)
{
    // 移除构造时已耗尽的子节点
    children_.erase(
        std::remove_if(children_.begin(), children_.end(),
                       [](const auto& c) { return c->isEnd(); }),
        children_.end());

    // 缓存静态上界（OR 语义：所有子节点上界之和）
    for (const auto& c : children_) max_score_ += c->maxScore();

    // 立即运行 WAND，将结果写入 score_order_hits_ 和 doc_order_hits_
    runWAND();

    // 初始化 Scorer 接口游标
    if (!doc_order_hits_.empty()) {
        cur_doc_   = doc_order_hits_[0].doc_id;
        cur_score_ = doc_order_hits_[0].score;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：过滤检查
// ─────────────────────────────────────────────────────────────────────────────

bool WANDScorer::passesFilter(DocId doc_id) const {
    if (!ctx_->filter) return true;
    const NumericFilter& f = *ctx_->filter;
    uint32_t idx = static_cast<uint32_t>(doc_id) - 1;
    if (f.hasPubtimeRange()) {
        int64_t pt = ctx_->seg.ffPubtime(idx);
        if (pt < f.pubtime_lo || pt > f.pubtime_hi) return false;
    }
    if (f.hasUidFilter()) {
        if (ctx_->seg.ffUid(idx) != f.uid) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// findPivot：在按 docId 升序排列的 children_ 中，从左到右累加 maxScore()，
// 直到累计值 >= theta；返回 pivot 下标，找不到返回 -1
// ─────────────────────────────────────────────────────────────────────────────

int WANDScorer::findPivot(float theta) const {
    float ub_sum = 0.f;
    for (int i = 0; i < (int)children_.size(); ++i) {
        ub_sum += children_[i]->maxScore();
        if (ub_sum >= theta) return i;
    }
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// alignLaggingCursors：将落后于 pivot_doc 的 children[0..pivot_idx-1] 推进
// ─────────────────────────────────────────────────────────────────────────────

void WANDScorer::alignLaggingCursors(int pivot_idx, DocId pivot_doc) {
    for (int i = 0; i < pivot_idx; ++i) {
        if (!children_[i]->isEnd() && children_[i]->docId() < pivot_doc)
            children_[i]->advance(pivot_doc);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// skipBlock：所有 cursor 跳过当前 Block（BlockMaxWAND 优化）
// ─────────────────────────────────────────────────────────────────────────────

void WANDScorer::skipBlock() {
    for (auto& c : children_) {
        DocId blk_end = c->blockMaxDocId();
        if (blk_end != INVALID_DOC) c->advance(blk_end + 1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// runWAND：WAND + BlockMaxWAND 主循环
//
// 维护一个大小为 top_k_ 的最小堆（min-heap by score），theta = 堆中最小分。
// 每轮：
//   1. 按 docId 排序，删除耗尽游标
//   2. findPivot(theta) → 找 pivot_doc
//   3a. min_doc == pivot_doc → 所有游标[0..pivot_idx]已对齐
//       · Block 级检查：block_ub_sum < theta → skipBlock()
//       · 否则精确打分，更新堆
//   3b. min_doc < pivot_doc → 落后游标 advance 到 pivot_doc
// ─────────────────────────────────────────────────────────────────────────────

void WANDScorer::runWAND() {
    if (top_k_ <= 0 || children_.empty()) return;

    struct HeapEntry {
        float score;
        DocId doc_id;
        bool operator<(const HeapEntry& o) const { return score > o.score; }
    };
    std::priority_queue<HeapEntry> heap;  // min-heap：堆顶是 top-K 中得分最低的
    float theta = 0.f;

    while (true) {
        // ── 排序 + 清理 ────────────────────────────────────────────────────
        std::sort(children_.begin(), children_.end(),
                  [](const auto& a, const auto& b) {
                      return a->docId() < b->docId();
                  });
        // 耗尽的游标（docId() == INVALID_DOC）排在最后，移除
        while (!children_.empty() && children_.back()->isEnd())
            children_.pop_back();
        if (children_.empty()) break;

        // ── pivot 选择 ──────────────────────────────────────────────────────
        int pivot_idx = findPivot(theta);
        if (pivot_idx < 0) break;  // 所有剩余文档的上界之和均 < theta，不可能入 top-K

        DocId pivot_doc = children_[pivot_idx]->docId();
        if (pivot_doc == INVALID_DOC) break;

        DocId min_doc = children_[0]->docId();

        if (min_doc == pivot_doc) {
            // ── 路径 A：所有游标[0..pivot_idx]对齐到 pivot_doc ───────────
            // BlockMaxWAND：当前 Block 上界之和不足 theta，整块跳过
            float block_ub_sum = 0.f;
            for (const auto& c : children_) block_ub_sum += c->blockMaxScore();

            if (block_ub_sum < theta) {
                skipBlock();
            } else {
                // 精确打分：对 pivot_doc 求所有子节点贡献之和
                if (ctx_->seg.isAlive(pivot_doc) && passesFilter(pivot_doc)) {
                    float s = 0.f;
                    for (auto& c : children_) {
                        if (c->docId() == pivot_doc) s += c->score();
                    }
                    // 维护 top-K 堆
                    if ((int)heap.size() < top_k_ || s > heap.top().score) {
                        if ((int)heap.size() == top_k_) heap.pop();
                        heap.push({s, pivot_doc});
                        if ((int)heap.size() == top_k_) theta = heap.top().score;
                    }
                }
                // 推进处于 pivot_doc 的所有游标
                for (auto& c : children_) {
                    if (!c->isEnd() && c->docId() == pivot_doc)
                        c->advance(pivot_doc + 1);
                }
            }
        } else {
            // ── 路径 B：min_doc < pivot_doc，落后游标追赶 ──────────────────
            alignLaggingCursors(pivot_idx, pivot_doc);
        }
    }

    // ── 堆 → 结果缓冲 ──────────────────────────────────────────────────────
    // heap 是最小堆，pop 顺序是 score 升序，reverse 得到降序
    score_order_hits_.reserve(heap.size());
    while (!heap.empty()) {
        score_order_hits_.push_back({heap.top().doc_id, heap.top().score});
        heap.pop();
    }
    std::reverse(score_order_hits_.begin(), score_order_hits_.end());

    // doc_order：按 doc_id 升序（供 Scorer 接口迭代）
    doc_order_hits_ = score_order_hits_;
    std::sort(doc_order_hits_.begin(), doc_order_hits_.end(),
              [](const Hit& a, const Hit& b) { return a.doc_id < b.doc_id; });
}

// ─────────────────────────────────────────────────────────────────────────────
// collectTopK：读 score_order_hits_，填充 stored 字段，返回 SearchResult 列表
// ─────────────────────────────────────────────────────────────────────────────

std::vector<SearchResult> WANDScorer::collectTopK() {
    std::vector<SearchResult> results;
    results.reserve(score_order_hits_.size());

    for (const auto& h : score_order_hits_) {
        auto stored = ctx_->seg.readStoredDoc(h.doc_id);
        SearchResult r;
        r.doc_id        = h.doc_id;
        r.score         = h.score;
        r.ext_id        = stored.ext_id;
        r.source        = stored.source();
        r.title         = stored.title();
        r.stored_fields = stored.str_fields;
        r.pubtime       = ctx_->seg.ffPubtime(static_cast<uint32_t>(h.doc_id) - 1);
        r.uid           = ctx_->seg.ffUid(static_cast<uint32_t>(h.doc_id) - 1);
        results.push_back(std::move(r));
    }
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scorer 接口：在 doc_order_hits_ 上顺序 / 跳跃迭代
// ─────────────────────────────────────────────────────────────────────────────

bool WANDScorer::next() {
    if (cur_idx_ + 1 >= doc_order_hits_.size()) {
        cur_doc_ = INVALID_DOC;
        cur_idx_ = doc_order_hits_.size();  // 防止下溢
        return false;
    }
    ++cur_idx_;
    cur_doc_   = doc_order_hits_[cur_idx_].doc_id;
    cur_score_ = doc_order_hits_[cur_idx_].score;
    return true;
}

bool WANDScorer::advance(DocId target) {
    // 在 doc_order_hits_[cur_idx_..] 中找第一个 doc_id >= target
    while (cur_idx_ < doc_order_hits_.size() &&
           doc_order_hits_[cur_idx_].doc_id < target)
        ++cur_idx_;

    if (cur_idx_ >= doc_order_hits_.size()) {
        cur_doc_ = INVALID_DOC;
        return false;
    }
    cur_doc_   = doc_order_hits_[cur_idx_].doc_id;
    cur_score_ = doc_order_hits_[cur_idx_].score;
    return true;
}

} // namespace ii
