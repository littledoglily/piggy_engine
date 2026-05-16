#include "postings/pfor_delta.h"
#include "postings/skiplist.h"
#include "../test_utils.h"
#include <iostream>
#include <vector>
#include <string>

using namespace ii;

void test_pfor_small() {
    TEST(pfor_small_list);
    std::vector<DocId> docs = {1, 3, 5, 10, 20, 50};
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes);
    auto decoded = PForDelta::decompress(
        compressed.data(), compressed.size(), docs.size());
    if (decoded != docs) FAIL("small list round-trip failed");
    PASS();
}

void test_pfor_128() {
    TEST(pfor_exact_128);
    std::vector<DocId> docs;
    docs.reserve(128);
    DocId cur = 1;
    for (int i = 0; i < 128; ++i) {
        docs.push_back(cur);
        cur += (i % 5) + 1;
    }
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes);
    assert(snodes.size() == 1);
    auto decoded = PForDelta::decompress(
        compressed.data(), compressed.size(), docs.size());
    if (decoded != docs) FAIL("128 doc round-trip failed");
    PASS();
}

void test_pfor_multi_block() {
    TEST(pfor_multi_block);
    std::vector<DocId> docs;
    docs.reserve(256);
    DocId cur = 1;
    for (int i = 0; i < 256; ++i) { docs.push_back(cur); cur += 2; }
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes);
    assert(snodes.size() == 2);
    auto decoded = PForDelta::decompress(
        compressed.data(), compressed.size(), docs.size());
    if (decoded != docs) FAIL("multi-block round-trip failed");
    PASS();
}

void test_pfor_exceptions() {
    TEST(pfor_exceptions);
    std::vector<DocId> docs = {1, 2, 3, 4, 5, 1000, 1001, 1002, 5000, 5001};
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes);
    auto decoded = PForDelta::decompress(
        compressed.data(), compressed.size(), docs.size());
    if (decoded != docs) FAIL("exception handling failed");
    PASS();
}

void test_pfor_compression_ratio() {
    TEST(pfor_compression_ratio);
    std::vector<DocId> docs;
    DocId cur = 1;
    for (int i = 0; i < 128; ++i) { docs.push_back(cur); cur += (i % 3) + 1; }
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes);
    size_t original = docs.size() * sizeof(DocId);
    if (compressed.size() >= original / 2) {
        FAIL("poor compression ratio: " + std::to_string(compressed.size())
             + " vs " + std::to_string(original));
    }
    PASS();
}

int main() {
    std::cout << "═══════════════════════════════════\n";
    std::cout << "  Postings / PForDelta Tests\n";
    std::cout << "═══════════════════════════════════\n\n";

    test_pfor_small();
    test_pfor_128();
    test_pfor_multi_block();
    test_pfor_exceptions();
    test_pfor_compression_ratio();

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
