#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// term_scorer.h  —  叶节点 Scorer，封装单个 field:term 的 PostingIterator
//
// 职责：
//   · 惰性迭代该 term 在指定字段的 posting list
//   · score()        = per-field BM25（调用 SegmentReader::bm25Score）
//   · maxScore()     = TermMeta::upper_bound × IDF（静态，构造时计算）
//   · blockMaxScore()= 当前 Block 的 max_tf_norm × IDF（随 Block 动态更新）
//
// field="" 时退回 legacy 模式（合并词典），仅供向后兼容。
// ─────────────────────────────────────────────────────────────────────────────
#include "query/scorer.h"
#include "query/scorer_context.h"
#include "postings/posting_iterator.h"
#include <string>

namespace ii {

class TermScorer : public Scorer {
public:
    // field="" 表示 legacy 模式（合并词典）
    TermScorer(const std::string& field,
               const std::string& term,
               const ScorerContext& ctx);

    DocId docId()         const override;
    bool  next()                override;
    bool  advance(DocId target) override;
    float score()               override;
    float maxScore()      const override;  // list_ub_（静态）
    float blockMaxScore() const override;  // iter_.blockMaxScore() × idf_（动态）
    DocId blockMaxDocId() const override;  // iter_.blockMaxDocId()
    bool  isEnd()         const override;

private:
    PostingIterator      iter_;
    std::string          field_;
    std::string          term_;
    float                idf_     = 0.0f;
    float                list_ub_ = 0.0f;  // upper_bound × IDF，构造时固定
    const ScorerContext* ctx_     = nullptr;
};

} // namespace ii
