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

// ─────────────────────────────────────────────────────────────────────────────
// 验证 BlockHeader.max_score 及 SkipNode.max_score 按 Block 独立存储
// 构造两个 Block，tf 分布不同，确保每个 Block 的 max_score != 全局 max_score
// ─────────────────────────────────────────────────────────────────────────────
void test_pfor_per_block_max_score() {
    TEST(pfor_per_block_max_score);

    // block0：128 个 doc，tf=1 → tf_norm = 1*(1.2+1)/(1+1.2) = 1.0
    // block1：128 个 doc，tf=5 → tf_norm = 5*(1.2+1)/(5+1.2) = 11.0/6.2 ≈ 1.774
    std::vector<DocId> docs;
    docs.reserve(256);
    for (int i = 0; i < 128; ++i) docs.push_back(static_cast<DocId>(i + 1));
    for (int i = 0; i < 128; ++i) docs.push_back(static_cast<DocId>(200 + i + 1));

    const float k1 = 1.2f;
    float blk0_max = 1.0f * (k1 + 1.0f) / (1.0f + k1);        // = 1.0
    float blk1_max = 5.0f * (k1 + 1.0f) / (5.0f + k1);        // ≈ 1.774
    float global_max = std::max(blk0_max, blk1_max);           // = blk1_max

    std::vector<float> block_ubs = {blk0_max, blk1_max};
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes, block_ubs);

    if (snodes.size() != 2) FAIL("expected 2 SkipNodes");

    constexpr float eps = 1e-5f;
    if (std::abs(snodes[0].max_score - blk0_max) > eps)
        FAIL("block0 max_score wrong: " + std::to_string(snodes[0].max_score));
    if (std::abs(snodes[1].max_score - blk1_max) > eps)
        FAIL("block1 max_score wrong: " + std::to_string(snodes[1].max_score));

    // 关键断言：block0 存的是局部值，而不是全局最大值
    if (std::abs(snodes[0].max_score - global_max) < eps)
        FAIL("block0 max_score should NOT equal global max");

    // round-trip 正确性不变
    auto decoded = PForDelta::decompress(compressed.data(), compressed.size(), docs.size());
    if (decoded != docs) FAIL("round-trip failed after per-block max_score");

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 验证空 block_max_tf_norms（默认参数）不会崩溃，max_score 全为 0
// ─────────────────────────────────────────────────────────────────────────────
void test_pfor_default_max_score() {
    TEST(pfor_default_max_score);
    std::vector<DocId> docs;
    for (int i = 0; i < 256; ++i) docs.push_back(static_cast<DocId>(i + 1));
    std::vector<SkipNode> snodes;
    auto compressed = PForDelta::compress(docs, snodes);  // 不传 block_ubs
    if (snodes.size() != 2) FAIL("expected 2 SkipNodes");
    for (size_t i = 0; i < snodes.size(); ++i) {
        if (snodes[i].max_score != 0.0f)
            FAIL("default max_score should be 0.0f for block " + std::to_string(i));
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
    test_pfor_per_block_max_score();
    test_pfor_default_max_score();

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
