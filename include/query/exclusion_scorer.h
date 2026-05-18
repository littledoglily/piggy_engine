#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// exclusion_scorer.h  —  MUST_NOT 过滤节点
//
// 迭代 main_ 的所有文档，跳过 excluded_ 中命中的文档。
// excluded_ 不贡献分数（score() / maxScore() 均来自 main_）。
//
// 算法（skipExcluded）：
//   1. 若 excluded_ 已耗尽，当前 doc 直接通过
//   2. 将 excluded_ advance 到 >= cur
//   3. 若 excluded_.docId() != cur，当前 doc 通过
//   4. 否则当前 doc 被过滤，让 main_ 推进到 cur+1，重复
// ─────────────────────────────────────────────────────────────────────────────
#include "query/scorer.h"
#include <memory>

namespace ii {

class ExclusionScorer : public Scorer {
public:
    ExclusionScorer(std::unique_ptr<Scorer> main,
                    std::unique_ptr<Scorer> excluded);

    DocId docId()         const override { return cur_doc_; }
    bool  next()                override;
    bool  advance(DocId target) override;
    float score()               override  { return main_->score(); }
    float maxScore()      const override  { return main_->maxScore(); }
    bool  isEnd()         const override  { return cur_doc_ == INVALID_DOC; }

private:
    // 从 cur 出发，找到第一个不在 excluded_ 中的 doc
    // 找到则设置 cur_doc_ 返回 true；main_ 耗尽则 cur_doc_=INVALID_DOC 返回 false
    bool skipExcluded(DocId cur);

    std::unique_ptr<Scorer> main_;
    std::unique_ptr<Scorer> excluded_;
    DocId cur_doc_ = INVALID_DOC;
};

} // namespace ii
