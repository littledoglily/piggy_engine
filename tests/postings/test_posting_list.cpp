#include "store/posting_list.h"
#include "index/index_writer.h"
#include "index/segment_writer.h"
#include "../test_utils.h"
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>

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

void test_append_mem_delta() {
    TEST(append_mem_delta);
    PostingList pl;

    // 新 doc：PostingEntry + DocId
    size_t d1 = pl.append(1, 0);
    assert(d1 == sizeof(PostingEntry) + sizeof(DocId));

    // 同 doc 重复 token：只新增一个 Pos
    size_t d2 = pl.append(1, 5);
    assert(d2 == sizeof(Pos));

    // 又一个新 doc
    size_t d3 = pl.append(3, 2);
    assert(d3 == sizeof(PostingEntry) + sizeof(DocId));

    PASS();
}

void test_in_memory_index_ram_usage() {
    TEST(in_memory_index_ram_usage);
    InMemoryIndex idx;

    // "python" doc1 — 新 term + 新 doc
    idx.addToken({"python", 1, 0, 0, 6, "body"});
    size_t expected = 6 + sizeof(PostingEntry) + sizeof(DocId);
    assert(idx.ramUsage() == expected);

    // "java" doc1 — 新 term + 新 doc
    idx.addToken({"java", 1, 1, 0, 4, "body"});
    expected += 4 + sizeof(PostingEntry) + sizeof(DocId);
    assert(idx.ramUsage() == expected);

    // "python" doc2 — 已有 term，新 doc
    idx.addToken({"python", 2, 0, 0, 6, "body"});
    expected += sizeof(PostingEntry) + sizeof(DocId);
    assert(idx.ramUsage() == expected);

    // "python" doc2 再次出现 — 已有 term，已有 doc，只加 Pos
    idx.addToken({"python", 2, 5, 30, 36, "body"});
    expected += sizeof(Pos);
    assert(idx.ramUsage() == expected);

    PASS();
}

void test_stored_doc_ram_usage() {
    TEST(stored_doc_ram_usage);
    StoredDoc sd;
    sd.doc_id = 1;
    sd.ext_id = 42;
    sd.str_fields["title"] = "hello";   // key=5, val=5
    sd.str_fields["body"]  = "world";   // key=4, val=5

    size_t expected = sizeof(DocId) + sizeof(uint64_t)  // doc_id + ext_id
                    + 5 + 5   // "title" + "hello"
                    + 4 + 5;  // "body"  + "world"
    assert(sd.ramUsage() == expected);
    PASS();
}

void test_index_writer_auto_flush() {
    TEST(index_writer_auto_flush);
    namespace fs = std::filesystem;
    const std::string dir = "/tmp/test_ram_flush";
    fs::remove_all(dir);

    // RAM 极小，每隔几个 doc 就应触发 flush
    Schema schema = Schema::defaultSchema();
    IndexWriter writer(dir, 0.001f /*1KB*/, schema);

    // 写入足够多的文档触发至少 1 次自动 flush
    for (int i = 1; i <= 50; ++i) {
        Document doc;
        doc.doc_id = static_cast<DocId>(i);
        doc.set("body", std::string("token") + std::to_string(i) +
                        " extra words to grow posting list size");
        writer.addDocument(doc);
    }
    writer.commit();

    assert(writer.segmentCount() > 1);  // 至少触发了一次 auto-flush
    fs::remove_all(dir);
    PASS();
}

int main() {
    std::cout << "═══════════════════════════════════\n";
    std::cout << "  Postings / PostingList Tests\n";
    std::cout << "═══════════════════════════════════\n\n";

    test_posting_list_append();
    test_in_memory_index();
    test_append_mem_delta();
    test_in_memory_index_ram_usage();
    test_stored_doc_ram_usage();
    test_index_writer_auto_flush();

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
