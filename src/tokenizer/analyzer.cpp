#include "tokenizer/analyzer.h"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace ii {

// ── 停用词表（常见英文停用词）────────────────────────────────────────────────
Analyzer::Analyzer() {
    stop_words_ = {
        "a","an","the","and","or","but","in","on","at","to","for",
        "of","with","by","from","is","are","was","were","be","been",
        "being","have","has","had","do","does","did","will","would",
        "could","should","may","might","shall","can","this","that",
        "these","those","it","its","i","my","we","our","you","your",
        "he","his","she","her","they","their","not","no","so","if",
        "as","up","out","about","into","than","then","there","when",
        "where","which","who","how","all","any","each","both","more",
    };
}

// ── 公开接口 ──────────────────────────────────────────────────────────────────

std::vector<Token> Analyzer::analyze(DocId doc_id, const std::string& text) const {
    // Step1: CharFilter
    std::string cleaned = charFilter(text);
    // Step2: Tokenize
    std::vector<std::string> raw_tokens = tokenize(cleaned);

    std::vector<Token> result;
    result.reserve(raw_tokens.size());

    Pos position = 0;
    uint32_t char_off = 0;

    for (const auto& raw : raw_tokens) {
        if (raw.empty()) continue;
        // Step3: StopFilter
        if (isStopWord(raw)) {
            char_off += static_cast<uint32_t>(raw.size()) + 1;
            continue;
        }
        // Step4: StemFilter
        std::string stemmed = stem(raw);
        if (stemmed.empty()) continue;

        Token tok;
        tok.term      = stemmed;
        tok.doc_id    = doc_id;
        tok.position  = position;
        tok.start_off = char_off;
        tok.end_off   = char_off + static_cast<uint32_t>(raw.size());

        result.push_back(std::move(tok));
        ++position;
        char_off += static_cast<uint32_t>(raw.size()) + 1;
    }
    return result;
}

std::vector<std::string> Analyzer::analyzeQuery(const std::string& query) const {
    std::string cleaned = charFilter(query);
    std::vector<std::string> raw = tokenize(cleaned);
    std::vector<std::string> result;
    for (const auto& r : raw) {
        if (r.empty() || isStopWord(r)) continue;
        std::string s = stem(r);
        if (!s.empty()) result.push_back(s);
    }
    return result;
}

// ── 私有实现 ──────────────────────────────────────────────────────────────────

std::string Analyzer::charFilter(const std::string& text) const {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (std::isalpha(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            out += ' ';  // 非字母替换为空格
        }
    }
    return out;
}

std::vector<std::string> Analyzer::tokenize(const std::string& text) const {
    std::vector<std::string> tokens;
    std::istringstream iss(text);
    std::string tok;
    while (iss >> tok) {
        tokens.push_back(tok);
    }
    return tokens;
}

bool Analyzer::isStopWord(const std::string& term) const {
    return stop_words_.count(term) > 0;
}

// 简化版 Porter Stemmer：去除常见英文后缀
std::string Analyzer::stem(const std::string& term) const {
    if (term.size() <= 3) return term;

    std::string s = term;

    // 去 "ing"
    if (s.size() > 4 && s.substr(s.size()-3) == "ing") {
        s = s.substr(0, s.size()-3);
        return s.empty() ? term : s;
    }
    // 去 "tion" → 保留词根
    if (s.size() > 5 && s.substr(s.size()-4) == "tion") {
        s = s.substr(0, s.size()-4);
        return s.empty() ? term : s;
    }
    // 去 "ness"
    if (s.size() > 5 && s.substr(s.size()-4) == "ness") {
        s = s.substr(0, s.size()-4);
        return s.empty() ? term : s;
    }
    // 去 "ment"
    if (s.size() > 5 && s.substr(s.size()-4) == "ment") {
        s = s.substr(0, s.size()-4);
        return s.empty() ? term : s;
    }
    // 去 "ers" / "es" / "ed" / "ly" / "er"
    if (s.size() > 4 && s.substr(s.size()-3) == "ers") {
        return s.substr(0, s.size()-3);
    }
    if (s.size() > 3 && s.substr(s.size()-2) == "es") {
        return s.substr(0, s.size()-2);
    }
    if (s.size() > 3 && s.substr(s.size()-2) == "ed") {
        return s.substr(0, s.size()-2);
    }
    if (s.size() > 3 && s.substr(s.size()-2) == "ly") {
        return s.substr(0, s.size()-2);
    }
    if (s.size() > 3 && s.substr(s.size()-2) == "er") {
        return s.substr(0, s.size()-2);
    }
    // 去结尾单 "s"（防止缩短到空）
    if (s.size() > 3 && s.back() == 's') {
        return s.substr(0, s.size()-1);
    }
    return s;
}

} // namespace ii
