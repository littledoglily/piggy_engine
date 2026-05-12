#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// analyzer.h  —  文本分析管道
//
// 流程：
//   原始文本
//     → CharFilter  （小写化、去标点）
//     → Tokenizer   （按空白切分）
//     → StopFilter  （去停用词）
//     → StemFilter  （Porter 词干提取，简化版）
//     → Token 流
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
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
    // ── Step 1: CharFilter ──────────────────────────────────────────────────
    // 将文本全部转小写，将非字母数字字符替换为空格
    std::string charFilter(const std::string& text) const;

    // ── Step 2: Tokenizer ───────────────────────────────────────────────────
    // 按空白切分为 token 列表
    std::vector<std::string> tokenize(const std::string& text) const;

    // ── Step 3: StopFilter ──────────────────────────────────────────────────
    // 去掉停用词，返回 false 表示该 token 应被丢弃
    bool isStopWord(const std::string& term) const;

    // ── Step 4: StemFilter ──────────────────────────────────────────────────
    // 简化版 Porter Stemmer：去除常见英文后缀
    std::string stem(const std::string& term) const;

    std::unordered_set<std::string> stop_words_;
};

} // namespace ii
