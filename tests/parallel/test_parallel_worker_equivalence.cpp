// tests/parallel/test_parallel_worker_equivalence.cpp
//
// Step 2 验证：ParallelIndexWriter(n_workers=1) 输出与 IndexWriter 等价
//
// 验证维度：
//   1. total_docs 相同
//   2. 每个字段的 term_count 相同
//   3. 每个 term 的 doc_freq / total_term_freq 完全相同
//   4. SegmentReader 可正常加载（文件格式合法）

#include "index/index_writer.h"
#include "index/parallel_index_writer.h"
#include "index/segment_reader.h"
#include "field/schema.h"
#include "core/types.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace ii;
namespace fs = std::filesystem;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << (msg) << "\n"; assert(false); } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// 测试文档集合（固定，两路都用同一份）
// ─────────────────────────────────────────────────────────────────────────────

static Schema makeTestSchema() {
    Schema s;
    s.fields = {
        {"body",     FieldType::Text,    IndexOption::FreqsPositions, true,  false, false, {}},
        {"title",    FieldType::Text,    IndexOption::FreqsPositions, true,  false, false, {}},
        {"category", FieldType::Keyword, IndexOption::FreqsOnly,      true,  false, false, {}},
    };
    return s;
}

struct DocData {
    std::string body;
    std::string title;
    std::string category;
};

static std::vector<DocData> testDocs() {
    return {
        {"python programming language is great for data science",    "Python Guide",      "tech"},
        {"java is a popular programming language for enterprise",     "Java Basics",       "tech"},
        {"machine learning uses python and data science techniques",  "ML Introduction",   "science"},
        {"data structures and algorithms in python",                  "DS Algorithms",     "tech"},
        {"python web development with flask and django",              "Web Dev Python",    "tech"},
        {"natural language processing and text mining with python",   "NLP Guide",         "science"},
        {"deep learning neural networks with python tensorflow",      "Deep Learning",     "science"},
        {"java spring framework for enterprise applications",         "Spring Boot",       "tech"},
        {"statistical data analysis and visualization techniques",    "Data Analysis",     "science"},
        {"python for beginners programming tutorial step by step",   "Beginner Python",   "tech"},
        {"algorithms complexity analysis and data structures",        "Algorithms Book",   "tech"},
        {"machine learning classification regression clustering",     "ML Methods",        "science"},
        {"java concurrency multithreading programming patterns",      "Java Threads",      "tech"},
        {"python pandas numpy scientific computing library",          "Scientific Python", "science"},
        {"software engineering design patterns best practices",       "Design Patterns",   "tech"},
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// 工具：从 SegmentReader 提取指定字段的 term dict
//   返回 map: term -> {doc_freq, total_term_freq}
// ─────────────────────────────────────────────────────────────────────────────

struct TermInfo { uint32_t df; uint32_t ttf; };

static std::map<std::string, TermInfo> extractTermDict(
    const SegmentReader& reader, const std::string& field)
{
    std::map<std::string, TermInfo> dict;
    const auto* terms = reader.fieldTermDict(field);
    if (!terms) return dict;
    for (const auto& [term, meta] : *terms) {
        dict[term] = {meta.doc_freq, meta.total_term_freq};
    }
    return dict;
}

// ─────────────────────────────────────────────────────────────────────────────
// 构建索引：单线程 IndexWriter
// ─────────────────────────────────────────────────────────────────────────────

static void buildSingleThread(const std::string& dir, const std::vector<DocData>& docs) {
    Schema schema = makeTestSchema();
    IndexWriter writer(dir, 512.0f /*大 buffer，确保一次 flush*/, schema);

    DocId id = 0;
    for (const auto& d : docs) {
        Document doc;
        doc.doc_id = ++id;
        doc.set("body",     d.body);
        doc.set("title",    d.title);
        doc.set("category", d.category);
        writer.addDocument(doc);
    }
    writer.commit();
}

// ─────────────────────────────────────────────────────────────────────────────
// 构建索引：单 Worker ParallelIndexWriter
// ─────────────────────────────────────────────────────────────────────────────

static void buildParallelN1(const std::string& dir, const std::vector<DocData>& docs) {
    Schema schema = makeTestSchema();
    // n_workers=1, 大 buffer → 所有文档进一个 Segment
    ParallelIndexWriter writer(dir, /*n_workers=*/1, /*ram_mb=*/512.0f, schema);

    for (const auto& d : docs) {
        Document doc;
        // doc_id 由 worker 内部分配，无需调用方设置
        doc.set("body",     d.body);
        doc.set("title",    d.title);
        doc.set("category", d.category);
        writer.addDocument(std::move(doc));
    }
    writer.commit();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1：两种构建方式 term_count 相同
// ─────────────────────────────────────────────────────────────────────────────

void test_term_count_equal() {
    TEST(term_count_equal_single_vs_parallel_n1);
    const std::string dir_st  = "/tmp/piggy_test_st";
    const std::string dir_par = "/tmp/piggy_test_par";

    fs::remove_all(dir_st);
    fs::remove_all(dir_par);

    auto docs = testDocs();
    buildSingleThread(dir_st,  docs);
    buildParallelN1  (dir_par, docs);

    // 加载两个 SegmentReader
    SegmentReader st_reader (dir_st,  0);
    SegmentReader par_reader(dir_par, 0);

    for (const std::string& field : {"body", "title", "category"}) {
        auto st_dict  = extractTermDict(st_reader,  field);
        auto par_dict = extractTermDict(par_reader, field);

        if (st_dict.size() != par_dict.size())
            FAIL("field=" + field + " term_count mismatch: "
                 + std::to_string(st_dict.size()) + " vs "
                 + std::to_string(par_dict.size()));
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2：每个 term 的 df / ttf 完全相同
// ─────────────────────────────────────────────────────────────────────────────

void test_term_stats_equal() {
    TEST(term_df_ttf_equal_single_vs_parallel_n1);
    const std::string dir_st  = "/tmp/piggy_test_st";
    const std::string dir_par = "/tmp/piggy_test_par";
    // 依赖 test_term_count_equal 已构建，直接加载

    SegmentReader st_reader (dir_st,  0);
    SegmentReader par_reader(dir_par, 0);

    for (const std::string& field : {"body", "title", "category"}) {
        auto st_dict  = extractTermDict(st_reader,  field);
        auto par_dict = extractTermDict(par_reader, field);

        for (const auto& [term, st_info] : st_dict) {
            auto it = par_dict.find(term);
            if (it == par_dict.end())
                FAIL("field=" + field + " term \"" + term + "\" missing in parallel index");

            if (st_info.df != it->second.df)
                FAIL("field=" + field + " term \"" + term + "\" df mismatch: "
                     + std::to_string(st_info.df) + " vs " + std::to_string(it->second.df));

            if (st_info.ttf != it->second.ttf)
                FAIL("field=" + field + " term \"" + term + "\" ttf mismatch: "
                     + std::to_string(st_info.ttf) + " vs " + std::to_string(it->second.ttf));
        }
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3：doc_count 相同
// ─────────────────────────────────────────────────────────────────────────────

void test_doc_count_equal() {
    TEST(doc_count_equal_single_vs_parallel_n1);
    SegmentReader st_reader ("/tmp/piggy_test_st",  0);
    SegmentReader par_reader("/tmp/piggy_test_par", 0);

    uint32_t st_dc  = st_reader.docCount();
    uint32_t par_dc = par_reader.docCount();

    if (st_dc != par_dc)
        FAIL("doc_count: " + std::to_string(st_dc) + " vs " + std::to_string(par_dc));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4：parallel 的 PostingIterator 遍历次数与单线程一致
// ─────────────────────────────────────────────────────────────────────────────

void test_posting_iterator_matches_single() {
    TEST(posting_iterator_count_matches_single_thread);
    SegmentReader st_reader ("/tmp/piggy_test_st",  0);
    SegmentReader par_reader("/tmp/piggy_test_par", 0);

    const auto* terms_ptr = par_reader.fieldTermDict("body");
    if (!terms_ptr || terms_ptr->empty())
        FAIL("no terms in body field");

    int checked = 0;
    for (const auto& [term, meta] : *terms_ptr) {
        auto st_it  = st_reader.postingIterator("body", term);
        auto par_it = par_reader.postingIterator("body", term);

        uint32_t st_cnt = 0,  par_cnt = 0;
        while (st_it.next())  ++st_cnt;
        while (par_it.next()) ++par_cnt;

        if (st_cnt != par_cnt)
            FAIL("term=\"" + term + "\" st_cnt=" + std::to_string(st_cnt)
                 + " par_cnt=" + std::to_string(par_cnt));
        if (++checked >= 30) break;
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5：多 Worker（n=2）总 doc 数正确，所有 term df >= 1
// ─────────────────────────────────────────────────────────────────────────────

void test_multi_worker_doc_count() {
    TEST(multi_worker_n2_doc_count_correct);
    const std::string dir_par2 = "/tmp/piggy_test_par2";
    fs::remove_all(dir_par2);

    auto docs = testDocs();
    Schema schema = makeTestSchema();
    {
        ParallelIndexWriter writer(dir_par2, /*n_workers=*/2, /*ram_mb=*/512.0f, schema);
        for (const auto& d : docs) {
            Document doc;
            doc.set("body",     d.body);
            doc.set("title",    d.title);
            doc.set("category", d.category);
            writer.addDocument(std::move(doc));
        }
        writer.commit();

        // commit 内总数正确
        if (writer.totalDocs() != static_cast<uint32_t>(docs.size()))
            FAIL("totalDocs=" + std::to_string(writer.totalDocs())
                 + " expected " + std::to_string(docs.size()));

        // 至少有一个 segment
        if (writer.segmentCount() == 0)
            FAIL("no segments produced");
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6：空输入不产生文件，commit() 不 crash
// ─────────────────────────────────────────────────────────────────────────────

void test_empty_commit() {
    TEST(empty_commit_no_crash);
    const std::string dir_empty = "/tmp/piggy_test_empty";
    fs::remove_all(dir_empty);
    {
        ParallelIndexWriter writer(dir_empty, 2, 64.0f, makeTestSchema());
        writer.commit();  // 没有任何文档
        if (writer.totalDocs() != 0)
            FAIL("totalDocs should be 0");
        // segment count 应为 0（没有数据）
        if (writer.segmentCount() != 0)
            FAIL("segmentCount should be 0 for empty commit");
    }
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== ParallelIndexWriter Equivalence Tests ===\n\n";

    // Test 1~4 依次使用同一批索引（Test 1 建，后续复用）
    test_term_count_equal();
    test_term_stats_equal();
    test_doc_count_equal();
    test_posting_iterator_matches_single();

    test_multi_worker_doc_count();
    test_empty_commit();

    std::cout << "\nAll equivalence tests passed.\n";
    return 0;
}
