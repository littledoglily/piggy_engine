#include "query/query_parser.h"
#include <sstream>

namespace ii {

QueryParser::QueryParser(const Analyzer& analyzer,
                         const std::vector<std::string>& default_fields)
    : analyzer_(analyzer), default_fields_(default_fields) {}

QueryParser::ParsedToken QueryParser::parseToken(const std::string& tok,
                                                  Occur default_occur) const {
    ParsedToken pt;
    std::string body = tok;

    if (!body.empty() && body[0] == '+') {
        pt.occur = Occur::MUST;
        body     = body.substr(1);
    } else if (!body.empty() && body[0] == '-') {
        pt.occur = Occur::MUST_NOT;
        body     = body.substr(1);
    } else {
        pt.occur = default_occur;
    }

    auto colon = body.find(':');
    if (colon != std::string::npos && colon > 0 && colon + 1 < body.size()) {
        pt.field    = body.substr(0, colon);
        pt.raw_term = body.substr(colon + 1);
    } else {
        pt.field    = "";
        pt.raw_term = body;
    }

    return pt;
}

std::unique_ptr<BooleanQuery> QueryParser::parse(const std::string& raw,
                                                   Occur default_occur) const {
    auto bq = std::make_unique<BooleanQuery>();

    std::istringstream iss(raw);
    std::string tok;
    while (iss >> tok) {
        auto pt = parseToken(tok, default_occur);
        if (pt.raw_term.empty()) continue;

        auto terms = analyzer_.analyzeQuery(pt.raw_term);
        if (terms.empty()) continue;  // 停用词，跳过

        for (const auto& term : terms) {
            if (!pt.field.empty()) {
                // field:term → 直接加入，保留 occur
                bq->add(std::make_unique<TermQuery>(pt.field, term), pt.occur);

            } else if (default_fields_.empty()) {
                // legacy 模式：裸 term，field=""
                bq->add(std::make_unique<TermQuery>("", term), pt.occur);

            } else if (pt.occur == Occur::SHOULD) {
                // SHOULD 裸 term → 展开为多个 SHOULD 子句直接加入外层
                for (const auto& f : default_fields_)
                    bq->add(std::make_unique<TermQuery>(f, term), Occur::SHOULD);

            } else {
                // MUST / MUST_NOT 裸 term → 包裹为内层 BooleanQuery(SHOULD 各字段)
                auto inner = std::make_unique<BooleanQuery>();
                for (const auto& f : default_fields_)
                    inner->add(std::make_unique<TermQuery>(f, term), Occur::SHOULD);
                bq->add(std::move(inner), pt.occur);
            }
        }
    }

    return bq;
}

} // namespace ii
