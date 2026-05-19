#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query_parser.h  —  将查询字符串解析为 BooleanQuery 树（Step 7）
//
// 语法规则（空格分隔 token）：
//   +term / +field:term   → MUST
//   -term / -field:term   → MUST_NOT
//   term  / field:term    → default_occur（默认 SHOULD）
//
// bare term（无 field 限定）展开规则：
//   SHOULD   → 直接在外层添加多个 SHOULD 子句（field1:term, field2:term, ...）
//   MUST     → 包裹为内层 BooleanQuery(SHOULD: field1:t, SHOULD: field2:t)，外层 MUST
//   MUST_NOT → 同上，外层 MUST_NOT
//
// 无默认字段时（legacy 模式）：裸 term 以 field="" 直接加入。
// ─────────────────────────────────────────────────────────────────────────────
#include "query/query.h"
#include "tokenizer/analyzer.h"
#include <string>
#include <vector>

namespace ii {

class QueryParser {
public:
    QueryParser(const Analyzer& analyzer,
                const std::vector<std::string>& default_fields);

    // 将原始查询字符串解析为 BooleanQuery 树
    // default_occur：无 +/- 前缀的 token 所用的 Occur（SHOULD=OR, MUST=AND）
    std::unique_ptr<BooleanQuery> parse(const std::string& raw,
                                        Occur default_occur = Occur::SHOULD) const;

private:
    struct ParsedToken {
        Occur       occur;
        std::string field;     // "" 表示裸 term
        std::string raw_term;  // 未经 Analyzer 处理的原始词
    };

    ParsedToken parseToken(const std::string& tok, Occur default_occur) const;

    const Analyzer&          analyzer_;
    std::vector<std::string> default_fields_;
};

} // namespace ii
