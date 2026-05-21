#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query/conjunction_scorer.h  —  AND 节点，N 路 Zigzag 求交集
// ─────────────────────────────────────────────────────────────────────────────
#include "query/scorer.h"
#include <vector>
#include <memory>

namespace ii {

class ConjunctionScorer : public Scorer {
public:
    explicit ConjunctionScorer(std::vector<std::unique_ptr<Scorer>> children);

    DocId docId()         const override { return cur_doc_; }
    bool  next()                override;
    bool  advance(DocId target) override;
    float score()               override;
    float maxScore()      const override;
    float blockMaxScore() const override;
    DocId blockMaxDocId() const override;
    bool  isEnd()         const override { return cur_doc_ == INVALID_DOC; }

private:
    bool doNext();

    std::vector<std::unique_ptr<Scorer>> children_;
    DocId cur_doc_ = INVALID_DOC;
};

} // namespace ii
