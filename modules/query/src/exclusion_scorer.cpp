#include "query/exclusion_scorer.h"

namespace ii {

ExclusionScorer::ExclusionScorer(std::unique_ptr<Scorer> main,
                                 std::unique_ptr<Scorer> excluded)
    : main_(std::move(main)), excluded_(std::move(excluded))
{
    if (!main_->isEnd()) {
        skipExcluded(main_->docId());
    }
}

// 从 cur 出发，跳过所有被 excluded_ 命中的 doc
// 前置：main_ 已定位到 cur（未耗尽）
// 后置：cur_doc_ 为第一个不被排除的 doc，或 INVALID_DOC
// excluded_ 是单调向前推进的，永远不会回退。两个指针的关系是：


// 情况 A：excluded_ 落后于 cur → advance 追赶
//          main_:     ... [cur] ...
//          excluded_: [?]  ↑ 需要追

// 情况 B：excluded_ 超过 cur → cur 安全通过
//          main_:     ... [cur] ...
//          excluded_:      [>cur]

// 情况 C：excluded_ 恰好等于 cur → 过滤，让 main_ 跳一步
//          main_:     ... [cur] ...
//          excluded_: ... [cur] ...
// 整个算法的时间复杂度是 O(N_main + N_excluded)，两个指针各自单调前进，不存在回退
bool ExclusionScorer::skipExcluded(DocId cur) {
    while (true) {
        if (excluded_->isEnd()) {
            cur_doc_ = cur;
            return true;
        }
        if (excluded_->docId() < cur) {
            if (!excluded_->advance(cur)) {
                cur_doc_ = cur;
                return true;
            }
        }
        // excluded_.docId() >= cur
        if (excluded_->docId() != cur) {
            cur_doc_ = cur;
            return true;
        }
        // cur 被过滤，推进 main_
        if (!main_->next()) {
            cur_doc_ = INVALID_DOC;
            return false;
        }
        cur = main_->docId();
    }
}

bool ExclusionScorer::next() {
    if (cur_doc_ == INVALID_DOC) return false;
    if (!main_->next()) {
        cur_doc_ = INVALID_DOC;
        return false;
    }
    return skipExcluded(main_->docId());
}

bool ExclusionScorer::advance(DocId target) {
    if (cur_doc_ == INVALID_DOC) return false;
    if (!main_->advance(target)) {
        cur_doc_ = INVALID_DOC;
        return false;
    }
    return skipExcluded(main_->docId());
}

} // namespace ii
