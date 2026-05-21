#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query/term_scorer.h  —  叶节点 Scorer，封装单个 field:term 的 PostingIterator
// ─────────────────────────────────────────────────────────────────────────────
#include "query/scorer.h"
#include "query/scorer_context.h"
#include "store/posting_iterator.h"
#include <string>

namespace ii {

class TermScorer : public Scorer {
public:
    TermScorer(const std::string& field,
               const std::string& term,
               const ScorerContext& ctx);

    DocId docId()         const override;
    bool  next()                override;
    bool  advance(DocId target) override;
    float score()               override;
    float maxScore()      const override;
    float blockMaxScore() const override;
    DocId blockMaxDocId() const override;
    bool  isEnd()         const override;

private:
    PostingIterator      iter_;
    std::string          field_;
    std::string          term_;
    float                idf_     = 0.0f;
    float                list_ub_ = 0.0f;
    const ScorerContext* ctx_     = nullptr;
};

} // namespace ii
