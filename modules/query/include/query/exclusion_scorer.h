#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query/exclusion_scorer.h  —  MUST_NOT 过滤节点
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
    bool skipExcluded(DocId cur);

    std::unique_ptr<Scorer> main_;
    std::unique_ptr<Scorer> excluded_;
    DocId cur_doc_ = INVALID_DOC;
};

} // namespace ii
