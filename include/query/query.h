#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query.h  —  Query 层：查询树的节点定义与 Scorer 工厂
//
// 层级关系：
//   Query ──► createScorer(ctx) ──► Scorer 树
//
// 叶节点 TermQuery   → TermScorer
// 复合节点 BooleanQuery 按子句类型自动路由：
//   全 MUST             → ConjunctionScorer
//   全 SHOULD           → WANDScorer（top_k 从 ctx.top_k 读取）
//   MUST (+ MUST_NOT)   → ConjunctionScorer / TermScorer，外包 ExclusionScorer
//   空查询              → nullptr（调用方需检查）
// ─────────────────────────────────────────────────────────────────────────────
#include "query/scorer.h"
#include "query/scorer_context.h"
#include <memory>
#include <string>
#include <vector>

namespace ii {

// ── 抽象基类 ─────────────────────────────────────────────────────────────────

class Query {
public:
    virtual ~Query() = default;

    // 为给定 Segment 构建 Scorer 树；空结果返回 nullptr
    virtual std::unique_ptr<Scorer> createScorer(const ScorerContext& ctx) const = 0;

    // 可读的调试字符串
    virtual std::string debugString() const = 0;

    // 递归收集树中所有叶节点的 (field, term) 对，供 IDF 预计算使用
    virtual void collectTerms(std::vector<std::pair<std::string,std::string>>& out) const = 0;

    Query()                        = default;
    Query(const Query&)            = delete;
    Query& operator=(const Query&) = delete;
};

// ── 叶节点：单 term 查询 ──────────────────────────────────────────────────────

class TermQuery : public Query {
public:
    // term 应已经过 Analyzer 处理（stem / lowercase）
    TermQuery(std::string field, std::string term);

    std::unique_ptr<Scorer> createScorer(const ScorerContext& ctx) const override;
    std::string debugString() const override;  // "field:term"
    void collectTerms(std::vector<std::pair<std::string,std::string>>& out) const override;

private:
    std::string field_;
    std::string term_;
};

// ── 布尔语义 ─────────────────────────────────────────────────────────────────

enum class Occur {
    MUST,      // 文档必须命中该子句（AND）
    SHOULD,    // 文档可选命中，命中则加分（OR）
    MUST_NOT,  // 文档不能命中该子句（排除）
};

struct BooleanClause {
    std::unique_ptr<Query> query;
    Occur                  occur;
};

// ── 复合节点 ─────────────────────────────────────────────────────────────────

class BooleanQuery : public Query {
public:
    void add(std::unique_ptr<Query> q, Occur occur);

    // 路由规则（见文件头注释）
    std::unique_ptr<Scorer> createScorer(const ScorerContext& ctx) const override;

    // 示例："(+body:python body:numpy -source:spam)"
    std::string debugString() const override;
    void collectTerms(std::vector<std::pair<std::string,std::string>>& out) const override;

    const std::vector<BooleanClause>& clauses() const { return clauses_; }

private:
    std::vector<BooleanClause> clauses_;
};

} // namespace ii
