#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// scorer.h  —  Scorer 抽象基类
//
// 所有查询节点（叶节点 TermScorer / 复合节点 ConjunctionScorer / WANDScorer /
// ExclusionScorer）都实现此接口，父节点无需感知子节点的具体类型。
//
// 迭代语义：
//   构造完成后迭代器指向第一个候选文档（或 isEnd()==true）。
//   next() / advance() 返回 false 时迭代器耗尽，此后行为未定义。
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"

namespace ii {

class Scorer {
public:
    virtual ~Scorer() = default;

    // 当前文档 ID；未 next() 前（或 isEnd()==true 时）行为未定义
    virtual DocId docId() const = 0;

    // 推进到下一个候选文档
    // 返回 false 表示耗尽（此后 isEnd()==true）
    virtual bool next() = 0;

    // 跳到第一个 docId >= target 的文档
    // 返回 false 表示不存在
    virtual bool advance(DocId target) = 0;

    // 当前文档的精确得分（BM25 或子节点得分聚合）
    virtual float score() = 0;

    // 当前迭代位置后可能出现的最高得分上界（WAND pivot 判断用）
    // MUST_NOT 节点不参与此上界
    virtual float maxScore() const = 0;

    // 当前 Block 内的得分上界（BlockMaxWAND 细筛用）
    // 默认返回 maxScore()；TermScorer 覆写为 block_max_score × IDF
    virtual float blockMaxScore() const { return maxScore(); }

    // 当前 Block 末尾的 doc_id（供父节点整块跳跃）
    // 默认返回 INVALID_DOC（不支持 Block 跳跃的节点使用此默认值）
    virtual DocId blockMaxDocId() const { return INVALID_DOC; }

    virtual bool isEnd() const = 0;

    // 不可拷贝，允许移动
    Scorer(const Scorer&)            = delete;
    Scorer& operator=(const Scorer&) = delete;
    Scorer()                         = default;
};

} // namespace ii
