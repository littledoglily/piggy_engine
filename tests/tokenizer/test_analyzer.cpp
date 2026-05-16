#include "tokenizer/analyzer.h"
#include "../test_utils.h"
#include <iostream>

using namespace ii;

void test_analyzer_basic() {
    TEST(analyzer_basic);
    Analyzer a;
    auto toks = a.analyze(1, "Python programming for beginners");
    bool has_for = false;
    for (const auto& t : toks) if (t.term == "for") has_for = true;
    if (has_for) FAIL("stop word 'for' not filtered");
    if (toks.empty()) FAIL("no tokens");
    PASS();
}

void test_analyzer_stemming() {
    TEST(analyzer_stemming);
    Analyzer a;
    auto toks = a.analyze(1, "programming programs");
    bool found = false;
    for (const auto& t : toks) {
        if (t.term.find("program") != std::string::npos) { found = true; break; }
    }
    if (!found) FAIL("stemming failed for 'programming'");
    PASS();
}

void test_analyzer_query() {
    TEST(analyzer_query);
    Analyzer a;
    auto terms = a.analyzeQuery("Python Tutorial AND Java");
    for (const auto& t : terms) {
        if (t == "and") FAIL("'and' should be filtered");
    }
    if (terms.size() < 2) FAIL("too few query terms");
    PASS();
}

int main() {
    std::cout << "═══════════════════════════════════\n";
    std::cout << "  Tokenizer / Analyzer Tests\n";
    std::cout << "═══════════════════════════════════\n\n";

    test_analyzer_basic();
    test_analyzer_stemming();
    test_analyzer_query();

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
