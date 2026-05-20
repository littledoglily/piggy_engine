#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// wand_scorer.h  —  OR TopK 节点，WAND 上界剪枝 + BlockMaxWAND 细筛
//
// 算法：
//   构造时立即运行 WAND 主循环，结果存入内部缓冲。
//   collectTopK() 读缓冲，按得分降序返回 SearchResult（含 stored 字段）。
//   Scorer 接口按 doc_id 升序暴露同一批结果，供父节点嵌套迭代。
//
//   maxScore() = Σ children[i]->maxScore()  （OR 语义，构造时缓存）
//   score()    = cur_score_（命中文档的精确 BM25 加和）
// ─────────────────────────────────────────────────────────────────────────────
#include "query/scorer.h"
#include "query/scorer_context.h"
#include "types.h"
#include <vector>
#include <memory>
#include <queue>

namespace ii {

class WANDScorer : public Scorer {
public:
    // children     : 每个子节点已指向第一个候选 doc（构造时自动跳过已耗尽的子节点）
    // top_k        : 返回得分最高的 K 篇文档；top_k <= 0 时直接返回空
    // use_block_max: true（默认）启用 BlockMaxWAND 细筛；false 退化为普通 WAND
    //                用于正确性对比测试（两者结果必须相同，前者更快）
    WANDScorer(std::vector<std::unique_ptr<Scorer>> children,
               int                                  top_k,
               const ScorerContext&                 ctx,
               bool                                 use_block_max = true);

    // 主接口：按得分降序返回 Top-K SearchResult（含 stored 字段和 FastField）
    std::vector<SearchResult> collectTopK();

    // ── Scorer 接口（供父节点嵌套）──────────────────────────────────────────
    // 按 doc_id 升序迭代；score() 返回对应文档的精确得分
    DocId docId()         const override { return cur_doc_; }
    bool  next()                override;
    bool  advance(DocId target) override;
    float score()               override { return cur_score_; }
    float maxScore()      const override { return max_score_; }
    // blockMaxScore / blockMaxDocId 使用基类默认（返回 maxScore / INVALID_DOC）
    bool  isEnd()         const override { return cur_idx_ >= doc_order_hits_.size(); }

    // Step 9：BlockMax 跳跃统计（用于验证 BlockMaxWAND 确实在剪枝）
    uint32_t blocksSkipped() const { return blocks_skipped_; }

private:
    // ── WAND 核心 ────────────────────────────────────────────────────────────
    void runWAND();

    // pivot 选择：children 已按 docId 升序排列；从左到右累加 maxScore() 直到 >= theta
    // 返回 pivot 下标，或 -1（所有上界之和 < theta）
    int  findPivot(float theta) const;

    // 将落后于 pivot_doc 的 children[0..pivot_idx-1] 全部 advance 到 pivot_doc
    void alignLaggingCursors(int pivot_idx, DocId pivot_doc);

    // Block 跳跃：所有 cursor 跳过当前 Block
    void skipBlock();

    // 数值过滤检查（ctx_->filter == nullptr 时总返回 true）
    bool passesFilter(DocId doc_id) const;

    // ── 数据 ──────────────────────────────────────────────────────────────────
    std::vector<std::unique_ptr<Scorer>> children_;  // 算法运行期间逐步推进
    int                  top_k_;
    const ScorerContext* ctx_;
    float                max_score_     = 0.f;  // Σ children maxScore()，构造时缓存
    bool                 use_block_max_ = true;  // 是否启用 BlockMaxWAND 细筛
    uint32_t             blocks_skipped_ = 0;    // 被 BlockMax 跳过的 Block 总数

    // ── 算法结果缓冲 ─────────────────────────────────────────────────────────
    struct Hit { DocId doc_id; float score; };
    std::vector<Hit> score_order_hits_;  // 按 score 降序（collectTopK 用）
    std::vector<Hit> doc_order_hits_;    // 按 doc_id 升序（Scorer 接口用）

    // ── Scorer 接口游标 ──────────────────────────────────────────────────────
    size_t cur_idx_   = 0;
    DocId  cur_doc_   = INVALID_DOC;
    float  cur_score_ = 0.f;
};

} // namespace ii
