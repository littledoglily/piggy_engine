#include "codec/skiplist.h"
#include "../test_utils.h"
#include <iostream>
#include <vector>

using namespace ii;

void test_skiplist_find() {
    TEST(skiplist_find);
    std::vector<SkipNode> nodes;
    for (int i = 0; i < 5; ++i) {
        SkipNode sn;
        sn.max_doc_id  = (DocId)((i + 1) * 128);
        sn.byte_offset = (uint64_t)(i * 200);
        sn.max_score   = 0.5f;
        sn.doc_count   = 128;
        nodes.push_back(sn);
    }
    SkipList sl(nodes);

    auto r1 = sl.find(1);
    assert(r1.block_index == 0);
    assert(r1.byte_offset == 0);

    auto r2 = sl.find(128);
    assert(r2.block_index == 0);

    auto r3 = sl.find(129);
    assert(r3.block_index == 1);

    auto r4 = sl.find(640);
    assert(r4.block_index == 4);

    auto r5 = sl.find(641);  // 超出范围
    assert(r5.byte_offset == UINT64_MAX);

    PASS();
}

void test_skiplist_serialize() {
    TEST(skiplist_serialize);
    std::vector<SkipNode> nodes;
    for (int i = 0; i < 3; ++i) {
        SkipNode sn;
        sn.max_doc_id  = (i + 1) * 100;
        sn.byte_offset = i * 80;
        sn.max_score   = 0.3f;
        sn.doc_count   = 100;
        nodes.push_back(sn);
    }
    SkipList sl(nodes);
    auto bytes = sl.serialize();
    auto sl2   = SkipList::deserialize(bytes.data(), bytes.size());

    assert(sl2.size() == sl.size());
    for (size_t i = 0; i < sl.size(); ++i) {
        assert(sl.node(i).max_doc_id  == sl2.node(i).max_doc_id);
        assert(sl.node(i).byte_offset == sl2.node(i).byte_offset);
    }
    PASS();
}

int main() {
    std::cout << "═══════════════════════════════════\n";
    std::cout << "  Postings / SkipList Tests\n";
    std::cout << "═══════════════════════════════════\n\n";

    test_skiplist_find();
    test_skiplist_serialize();

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
