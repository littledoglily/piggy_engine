#include "store/posting_list.h"
#include "../test_utils.h"
#include <iostream>
#include <vector>
#include <string>

using namespace ii;

void test_posting_list_append() {
    TEST(posting_list_append);
    PostingList pl;
    pl.append(1, 0);
    pl.append(1, 5);  // 同一 doc 第二次出现，累加 tf
    pl.append(3, 2);
    pl.append(5, 1);

    assert(pl.size() == 3);  // 3 个唯一 doc

    const auto& entries = pl.entries();
    assert(entries[0].doc_id == 1);
    assert(entries[0].tf     == 2);
    assert(entries[0].positions.size() == 2);
    assert(entries[1].doc_id == 3);
    assert(entries[1].tf     == 1);

    auto ids = pl.docIds();
    assert(ids.size() == 3);
    assert(ids[0] == 1 && ids[1] == 3 && ids[2] == 5);
    PASS();
}

void test_in_memory_index() {
    TEST(in_memory_index);
    InMemoryIndex idx;
    Token t1{"python", 1, 0, 0, 6};
    Token t2{"java",   1, 1, 7, 11};
    Token t3{"python", 2, 0, 0, 6};
    Token t4{"python", 2, 5, 30, 36};

    idx.addToken(t1);
    idx.addToken(t2);
    idx.addToken(t3);
    idx.addToken(t4);

    assert(idx.termCount() == 2);

    const PostingList* pl_py = idx.getPostingList("python");
    assert(pl_py != nullptr);
    assert(pl_py->size() == 2);

    const PostingList* pl_ja = idx.getPostingList("java");
    assert(pl_ja != nullptr);
    assert(pl_ja->size() == 1);

    auto terms = idx.sortedTerms();
    assert(terms[0] == "java");
    assert(terms[1] == "python");
    PASS();
}

int main() {
    std::cout << "═══════════════════════════════════\n";
    std::cout << "  Postings / PostingList Tests\n";
    std::cout << "═══════════════════════════════════\n\n";

    test_posting_list_append();
    test_in_memory_index();

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
