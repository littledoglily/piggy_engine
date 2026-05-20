#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// analysis/analyzer.h  —  文本分析管道
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include <unordered_set>

namespace ii {

class Analyzer {
public:
    Analyzer();

    // 对一篇文档的文本做全量分析，返回 Token 列表
    std::vector<Token> analyze(DocId doc_id, const std::string& text) const;

    // 对查询字符串做分析（不含位置信息），返回 term 列表
    std::vector<std::string> analyzeQuery(const std::string& query) const;

private:
    std::string              charFilter(const std::string& text) const;
    std::vector<std::string> tokenize(const std::string& text) const;
    bool                     isStopWord(const std::string& term) const;
    std::string              stem(const std::string& term) const;

    std::unordered_set<std::string> stop_words_;
};

} // namespace ii
