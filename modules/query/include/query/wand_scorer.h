#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query/wand_scorer.h  —  OR TopK 节点，WAND 上界剪枝 + BlockMaxWAND 细筛
// ─────────────────────────────────────────────────────────────────────────────
#include "query/scorer.h"
#include "query/scorer_context.h"
#include "core/types.h"
#include <vector>
#include <memory>
#include <queue>

namespace ii {

class WANDScorer : public Scorer {
public:
    WANDScorer(std::vector<std::unique_ptr<Scorer>> children,
               int                                  top_k,
               const ScorerContext&                 ctx,
               bool                                 use_block_max = true);

    // 按得分降序返回 Top-K，stored 字段由调用方（IndexSearcher）填充
    std::vector<SearchResult> collectTopK();

    DocId docId()         const override { return cur_doc_; }
    bool  next()                override;
    bool  advance(DocId target) override;
    float score()               override { return cur_score_; }
    float maxScore()      const override { return max_score_; }
    bool  isEnd()         const override { return cur_idx_ >= doc_order_hits_.size(); }

    uint32_t blocksSkipped() const { return blocks_skipped_; }

private:
    void runWAND();
    int  findPivot(float theta) const;
    void alignLaggingCursors(int pivot_idx, DocId pivot_doc);
    void skipBlock();
    bool passesFilter(DocId doc_id) const;

    std::vector<std::unique_ptr<Scorer>> children_;
    int                  top_k_;
    const ScorerContext* ctx_;
    float                max_score_     = 0.f;
    bool                 use_block_max_ = true;
    uint32_t             blocks_skipped_ = 0;

    struct Hit { DocId doc_id; float score; };
    std::vector<Hit> score_order_hits_;
    std::vector<Hit> doc_order_hits_;

    size_t cur_idx_   = 0;
    DocId  cur_doc_   = INVALID_DOC;
    float  cur_score_ = 0.f;
};

} // namespace ii
