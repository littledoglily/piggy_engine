#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query/query.h  —  Query 层：查询树节点定义与 Scorer 工厂
// ─────────────────────────────────────────────────────────────────────────────
#include "query/scorer.h"
#include "query/scorer_context.h"
#include <memory>
#include <string>
#include <vector>

namespace ii {

class Query {
public:
    virtual ~Query() = default;
    virtual std::unique_ptr<Scorer> createScorer(const ScorerContext& ctx) const = 0;
    virtual std::string debugString() const = 0;
    virtual void collectTerms(std::vector<std::pair<std::string,std::string>>& out) const = 0;

    Query()                        = default;
    Query(const Query&)            = delete;
    Query& operator=(const Query&) = delete;
};

class TermQuery : public Query {
public:
    TermQuery(std::string field, std::string term);
    std::unique_ptr<Scorer> createScorer(const ScorerContext& ctx) const override;
    std::string debugString() const override;
    void collectTerms(std::vector<std::pair<std::string,std::string>>& out) const override;

private:
    std::string field_;
    std::string term_;
};

enum class Occur {
    MUST,
    SHOULD,
    MUST_NOT,
};

struct BooleanClause {
    std::unique_ptr<Query> query;
    Occur                  occur;
};

class BooleanQuery : public Query {
public:
    void add(std::unique_ptr<Query> q, Occur occur);
    std::unique_ptr<Scorer> createScorer(const ScorerContext& ctx) const override;
    std::string debugString() const override;
    void collectTerms(std::vector<std::pair<std::string,std::string>>& out) const override;
    const std::vector<BooleanClause>& clauses() const { return clauses_; }

private:
    std::vector<BooleanClause> clauses_;
};

} // namespace ii
