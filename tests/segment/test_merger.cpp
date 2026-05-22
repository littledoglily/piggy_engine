#include "index/segment_merger.h"
#include "index/segment_reader.h"
#include "index/index_writer.h"
#include "query/index_searcher.h"
#include "../test_utils.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <string>

using namespace ii;

void test_soft_delete_and_merge() {
    TEST(soft_delete_and_merge);
    const std::string dir = "/tmp/ii_merge_test";
    std::filesystem::remove_all(dir);

    {
        IndexWriter writer(dir, 0.01f);
        for (int i = 1; i <= 10; ++i) {
            Document doc;
            doc.doc_id   = i;
            doc.set("title", "Doc " + std::to_string(i));
            doc.set("body", "python tutorial data science machine learning " + std::to_string(i));
            doc.set("category", "test");
            writer.addDocument(doc);
        }
        writer.commit();
    }

    {
        SegmentMerger merger(dir);
        int deleted = merger.softDeleteBatch({1, 3, 5});
        assert(deleted == 3);
        assert(!merger.isAlive(1));
        assert(!merger.isAlive(3));
        assert( merger.isAlive(2));
        assert( merger.isAlive(4));

        auto stats = merger.mergeAll(50);
        assert(stats.deleted_doc_count == 3);
        assert(stats.output_doc_count  == 7);
    }

    {
        IndexSearcher searcher(dir);
        auto results = searcher.search("python", 10, QueryMode::OR);
        for (const auto& r : results) {
            bool is_deleted_title =
                r.title == "Doc 1" || r.title == "Doc 3" || r.title == "Doc 5";
            if (is_deleted_title) FAIL("deleted doc appeared in results");
        }
    }

    std::filesystem::remove_all(dir);
    PASS();
}

void test_merge_tf_and_positions() {
    TEST(merge_tf_and_positions);
    const std::string dir = "/tmp/ii_tf_pos_test";
    std::filesystem::remove_all(dir);

    {
        IndexWriter writer(dir, 100.0f);

        Document d1;
        d1.doc_id = 1; d1.set("title", "Multi Python");
        d1.set("body", "python python python"); d1.set("category", "test");
        writer.addDocument(d1);

        Document d2;
        d2.doc_id = 2; d2.set("title", "Single Python");
        d2.set("body", "python tutorial"); d2.set("category", "test");
        writer.addDocument(d2);

        writer.flush();  // → segment 0

        Document d3;
        d3.doc_id = 3; d3.set("title", "Double Python");
        d3.set("body", "python python guide"); d3.set("category", "test");
        writer.addDocument(d3);

        writer.commit();  // → segment 1
    }

    uint32_t merged_seg_id = 0;
    {
        SegmentMerger merger(dir);
        auto ids = merger.activeSegmentIds();
        if (ids.size() < 2) FAIL("expected 2 segments before merge");
        auto stats = merger.mergeAll(99);
        merged_seg_id = stats.output_segment_id;
        if (stats.output_doc_count != 3) FAIL("expected 3 docs after merge");
    }

    {
        SegmentReader reader(dir, merged_seg_id);

        auto entries = reader.readPosEntries("python");
        if (entries.size() != 3)
            FAIL("expected 3 docs with python, got " + std::to_string(entries.size()));

        std::sort(entries.begin(), entries.end(),
                  [](const PostingEntry& a, const PostingEntry& b) {
                      return a.doc_id < b.doc_id;
                  });

        // title+body 拼接分析后 python 出现次数：doc1→4, doc2→2, doc3→3
        uint32_t expected_tf[3] = {4, 2, 3};
        for (int i = 0; i < 3; ++i) {
            if (entries[i].tf != expected_tf[i])
                FAIL("doc" + std::to_string(i+1) + " tf expected "
                     + std::to_string(expected_tf[i]) + ", got "
                     + std::to_string(entries[i].tf));
            if (entries[i].positions.size() != expected_tf[i])
                FAIL("doc" + std::to_string(i+1) + " positions count mismatch");
            for (uint32_t j = 1; j < entries[i].positions.size(); ++j) {
                if (entries[i].positions[j] <= entries[i].positions[j-1])
                    FAIL("doc" + std::to_string(i+1) + " positions not ascending");
            }
        }

        const TermMeta* meta = reader.getTermMeta("python");
        if (!meta) FAIL("python not found in term dict");
        if (meta->total_term_freq != 9)
            FAIL("total_term_freq expected 9, got " + std::to_string(meta->total_term_freq));
    }

    std::filesystem::remove_all(dir);
    PASS();
}

int main() {
    std::cout << "═══════════════════════════════════\n";
    std::cout << "  Segment / Merger Tests\n";
    std::cout << "═══════════════════════════════════\n\n";

    test_soft_delete_and_merge();
    test_merge_tf_and_positions();

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
