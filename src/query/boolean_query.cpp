#include "query/query.h"
#include "query/term_scorer.h"
#include "query/conjunction_scorer.h"
#include "query/wand_scorer.h"
#include "query/exclusion_scorer.h"

namespace ii {

// ── TermQuery ────────────────────────────────────────────────────────────────

TermQuery::TermQuery(std::string field, std::string term)
    : field_(std::move(field)), term_(std::move(term)) {}

std::unique_ptr<Scorer> TermQuery::createScorer(const ScorerContext& ctx) const {
    return std::make_unique<TermScorer>(field_, term_, ctx);
}

std::string TermQuery::debugString() const {
    return field_.empty() ? term_ : field_ + ":" + term_;
}

void TermQuery::collectTerms(
    std::vector<std::pair<std::string,std::string>>& out) const {
    out.push_back({field_, term_});
}

// ── BooleanQuery ─────────────────────────────────────────────────────────────

void BooleanQuery::add(std::unique_ptr<Query> q, Occur occur) {
    clauses_.push_back({std::move(q), occur});
}

std::unique_ptr<Scorer> BooleanQuery::createScorer(const ScorerContext& ctx) const {
    std::vector<std::unique_ptr<Scorer>> must_scorers;
    std::vector<std::unique_ptr<Scorer>> should_scorers;
    std::vector<std::unique_ptr<Scorer>> must_not_scorers;

    for (const auto& clause : clauses_) {
        auto scorer = clause.query->createScorer(ctx);
        // nullptr 或已耗尽的 MUST 子句 → 整个查询无结果
        if (!scorer || scorer->isEnd()) {
            if (clause.occur == Occur::MUST) return nullptr;
            continue;  // 空的 SHOULD / MUST_NOT 直接忽略
        }
        switch (clause.occur) {
            case Occur::MUST:     must_scorers.push_back(std::move(scorer));     break;
            case Occur::SHOULD:   should_scorers.push_back(std::move(scorer));   break;
            case Occur::MUST_NOT: must_not_scorers.push_back(std::move(scorer)); break;
        }
    }

    std::unique_ptr<Scorer> root;

    if (must_scorers.empty() && !should_scorers.empty()) {
        // 全 SHOULD → WANDScorer，top_k 来自 ScorerContext
        root = std::make_unique<WANDScorer>(std::move(should_scorers), ctx.top_k, ctx);

    } else if (!must_scorers.empty()) {
        // MUST → 单节点直通 or ConjunctionScorer
        if (must_scorers.size() == 1) {
            root = std::move(must_scorers[0]);
        } else {
            root = std::make_unique<ConjunctionScorer>(std::move(must_scorers));
        }
        // 注意：SHOULD 在 MUST 存在时暂不参与（Step 6 简化，提分逻辑留至后续）

    } else {
        // 无 MUST 也无 SHOULD（仅 MUST_NOT 或完全为空）→ 无意义查询
        return nullptr;
    }

    // MUST_NOT：逐个链式包裹 ExclusionScorer
    // 多个 MUST_NOT 等价于 OR 排除：先排 excl[0]，再排 excl[1]，…
    for (auto& excl : must_not_scorers) {
        root = std::make_unique<ExclusionScorer>(std::move(root), std::move(excl));
    }

    return root;
}

void BooleanQuery::collectTerms(
    std::vector<std::pair<std::string,std::string>>& out) const {
    for (const auto& c : clauses_) c.query->collectTerms(out);
}

std::string BooleanQuery::debugString() const {
    std::string s = "(";
    bool first = true;
    for (const auto& c : clauses_) {
        if (!first) s += ' ';
        first = false;
        switch (c.occur) {
            case Occur::MUST:     s += '+'; break;
            case Occur::MUST_NOT: s += '-'; break;
            case Occur::SHOULD:              break;
        }
        s += c.query->debugString();
    }
    s += ')';
    return s;
}

} // namespace ii
