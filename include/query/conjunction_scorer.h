#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// conjunction_scorer.h  —  AND 节点，N 路 Zigzag 求交集
//
// 算法：
//   构造时按 maxScore() 降序排列子节点（maxScore 越大 df 越小），
//   children_[0] 作为 lead（df 最小，最短 posting list 驱动迭代）。
//   doNext() 从 lead 当前位置出发，让其余子节点依次 advance() 追赶，
//   直到所有节点对齐到同一 doc_id。
//
// 接口语义：
//   score()        = Σ children[i]->score()
//   maxScore()     = Σ children[i]->maxScore()（AND 中所有 term 都贡献）
//   blockMaxScore()= Σ children[i]->blockMaxScore()
//   blockMaxDocId()= min(children[i]->blockMaxDocId())（最早的 Block 边界）
// ─────────────────────────────────────────────────────────────────────────────
#include "query/scorer.h"
#include <vector>
#include <memory>

namespace ii {

class ConjunctionScorer : public Scorer {
public:
    // children 至少含 1 个元素；构造后迭代器已指向第一个命中 doc
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
    // 从 children_[0] 当前位置起，找到第一个所有子节点对齐的 doc
    // 找到则设置 cur_doc_ 并返回 true；耗尽则 cur_doc_=INVALID_DOC，返回 false
    bool doNext();

    std::vector<std::unique_ptr<Scorer>> children_;
    DocId cur_doc_ = INVALID_DOC;
};

} // namespace ii
