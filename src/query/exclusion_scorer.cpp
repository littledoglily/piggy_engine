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
        if (!main_->advance(cur + 1)) {
            cur_doc_ = INVALID_DOC;
            return false;
        }
        cur = main_->docId();
    }
}

bool ExclusionScorer::next() {
    if (cur_doc_ == INVALID_DOC) return false;
    if (!main_->advance(cur_doc_ + 1)) {
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
