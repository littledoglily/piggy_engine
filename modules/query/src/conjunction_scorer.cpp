#include "query/conjunction_scorer.h"
#include <algorithm>
#include <numeric>

namespace ii {

ConjunctionScorer::ConjunctionScorer(std::vector<std::unique_ptr<Scorer>> children)
    : children_(std::move(children))
{
    // maxScore() 降序排列：maxScore 越大通常 df 越小，使 children_[0] 成为 lead
    std::sort(children_.begin(), children_.end(),
              [](const auto& a, const auto& b) {
                  return a->maxScore() > b->maxScore();
              });

    // 任意子节点已耗尽，整个交集为空
    for (const auto& c : children_) {
        if (c->isEnd()) return;
    }

    doNext();
}

// ── Zigzag AND 内核 ───────────────────────────────────────────────────────────
// 前置条件：children_[0] 已定位到某个 doc（未耗尽）
// 后置条件：cur_doc_ 为第一个所有子节点对齐的 doc，或 INVALID_DOC（耗尽）
bool ConjunctionScorer::doNext() {
    while (!children_[0]->isEnd()) {
        DocId target    = children_[0]->docId();
        bool  all_match = true;

        for (size_t i = 1; i < children_.size(); ++i) {
            if (!children_[i]->advance(target)) {
                cur_doc_ = INVALID_DOC;
                return false;
            }
            if (children_[i]->docId() != target) {
                // children_[i] 跳过了 target，让 lead 追赶
                if (!children_[0]->advance(children_[i]->docId())) {
                    cur_doc_ = INVALID_DOC;
                    return false;
                }
                all_match = false;
                break;
            }
        }

        if (all_match) {
            cur_doc_ = target;
            return true;
        }
        // lead 已更新，继续外层 while
    }

    cur_doc_ = INVALID_DOC;
    return false;
}

bool ConjunctionScorer::next() {
    if (cur_doc_ == INVALID_DOC) return false;
    // 让 lead 跳过当前 doc，再找下一个匹配
    if (!children_[0]->advance(cur_doc_ + 1)) {
        cur_doc_ = INVALID_DOC;
        return false;
    }
    return doNext();
}

bool ConjunctionScorer::advance(DocId target) {
    if (!children_[0]->advance(target)) {
        cur_doc_ = INVALID_DOC;
        return false;
    }
    return doNext();
}

float ConjunctionScorer::score() {
    float s = 0.f;
    for (auto& c : children_) s += c->score();
    return s;
}

float ConjunctionScorer::maxScore() const {
    float s = 0.f;
    for (const auto& c : children_) s += c->maxScore();
    return s;
}

float ConjunctionScorer::blockMaxScore() const {
    float s = 0.f;
    for (const auto& c : children_) s += c->blockMaxScore();
    return s;
}

DocId ConjunctionScorer::blockMaxDocId() const {
    DocId d = INVALID_DOC;
    for (const auto& c : children_) {
        DocId bd = c->blockMaxDocId();
        if (bd < d) d = bd;
    }
    return d;
}

} // namespace ii
