#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query/query_parser.h  —  将查询字符串解析为 BooleanQuery 树
// ─────────────────────────────────────────────────────────────────────────────
#include "query/query.h"
#include "analysis/analyzer.h"
#include <string>
#include <vector>

namespace ii {

class QueryParser {
public:
    QueryParser(const Analyzer& analyzer,
                const std::vector<std::string>& default_fields);

    std::unique_ptr<BooleanQuery> parse(const std::string& raw,
                                        Occur default_occur = Occur::SHOULD) const;

private:
    struct ParsedToken {
        Occur       occur;
        std::string field;
        std::string raw_term;
    };

    ParsedToken parseToken(const std::string& tok, Occur default_occur) const;

    const Analyzer&          analyzer_;
    std::vector<std::string> default_fields_;
};

} // namespace ii
