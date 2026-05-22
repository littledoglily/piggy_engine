// tests/segment/test_per_field_index.cpp
// Per-field 倒排索引端到端测试（Step 8 / 设计文档第九节）
//
// 测试范围：
//   1. 写入验证：per-field 文件生成正确（.tim_title/.doc_title/.pos_title, .tim_body/.doc_body, 无 .pos_body）
//   2. 字段隔离：同一 term 在两个字段中 df 各自独立
//   3. 字段查询：search("title:python") 只匹配 title; bare term 展开到所有字段
//   4. Merge 验证：mergeAll() 后 per-field 文件保留且 df 正确
//   5. Legacy 兼容：default schema 的旧接口（deprecated API）仍能访问 per-field 数据

#include "index/index_writer.h"
#include "index/segment_reader.h"
#include "index/segment_merger.h"
#include "query/index_searcher.h"
#include "field/schema.h"
#include "../test_utils.h"

#include <filesystem>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace ii;
namespace fs = std::filesystem;

static std::string tmpDir(const std::string& suffix) {
    std::string dir = "/tmp/test_pf_" + suffix;
    fs::remove_all(dir);
    return dir;
}

// Schema with title(FreqsPositions) + body(FreqsOnly) + pubtime(fast)
static Schema makeSchema() {
    Schema s;
    s.fields = {
        {"title",   FieldType::Text,  IndexOption::FreqsPositions, true,  false},
        {"body",    FieldType::Text,  IndexOption::FreqsOnly,      false, false},
        {"pubtime", FieldType::Int64, IndexOption::None,           false, true },
    };
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1：写入验证 — per-field 文件生成，FreqsOnly 不产生 .pos_<field>
// ─────────────────────────────────────────────────────────────────────────────
void test_write_per_field_files() {
    TEST(write_per_field_files);
    const std::string dir = tmpDir("write");

    {
        IndexWriter writer(dir, 100.f, makeSchema());
        Document doc;
        doc.doc_id = 1;
        doc.set("title", "Python tutorial");
        doc.set("body",  "Python data science");
        doc.set("pubtime", int64_t(1000));
        writer.addDocument(doc);
        writer.commit();
    }

    // FreqsPositions → segment_0/tim_title, doc_title, pos_title 均存在
    if (!fs::exists(dir + "/segment_0/tim_title")) FAIL("missing segment_0/tim_title");
    if (!fs::exists(dir + "/segment_0/doc_title")) FAIL("missing segment_0/doc_title");
    if (!fs::exists(dir + "/segment_0/pos_title")) FAIL("missing segment_0/pos_title");

    // FreqsOnly → segment_0/tim_body, doc_body 存在，pos_body 不存在
    if (!fs::exists(dir + "/segment_0/tim_body")) FAIL("missing segment_0/tim_body");
    if (!fs::exists(dir + "/segment_0/doc_body")) FAIL("missing segment_0/doc_body");
    if (fs::exists(dir + "/segment_0/pos_body"))  FAIL("unexpected segment_0/pos_body for FreqsOnly field");

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2：字段隔离 — 同一 term 在两个字段中 df 各自独立
// ─────────────────────────────────────────────────────────────────────────────
void test_field_isolation() {
    TEST(field_isolation);
    const std::string dir = tmpDir("isolation");

    {
        IndexWriter writer(dir, 100.f, makeSchema());
        // doc 1: title 有 python，body 有 python
        Document d1; d1.doc_id = 1;
        d1.set("title", "python");
        d1.set("body",  "python programming");
        writer.addDocument(d1);

        // doc 2: title 无 python，body 有 python
        Document d2; d2.doc_id = 2;
        d2.set("title", "java tutorial");
        d2.set("body",  "python tutorial");
        writer.addDocument(d2);

        writer.commit();
    }

    SegmentReader seg(dir, 0);

    // title 中 python 只出现在 doc 1
    const TermMeta* title_meta = seg.getTermMeta("title", "python");
    if (!title_meta)                  FAIL("title:python not found");
    if (title_meta->doc_freq != 1)    FAIL("title:python df should be 1");

    // body 中 python 出现在 doc 1 和 doc 2
    const TermMeta* body_meta = seg.getTermMeta("body", "python");
    if (!body_meta)                   FAIL("body:python not found");
    if (body_meta->doc_freq != 2)     FAIL("body:python df should be 2");

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3：字段查询 — field:term 只命中指定字段，bare term 展开到所有字段
// ─────────────────────────────────────────────────────────────────────────────
void test_field_query() {
    TEST(field_query);
    const std::string dir = tmpDir("query");

    {
        IndexWriter writer(dir, 100.f, makeSchema());
        // doc 1: title 有 python
        Document d1; d1.doc_id = 1;
        d1.set("title", "python language");
        d1.set("body",  "general programming");
        writer.addDocument(d1);

        // doc 2: body 有 python，title 无
        Document d2; d2.doc_id = 2;
        d2.set("title", "java programming");
        d2.set("body",  "python tutorial");
        writer.addDocument(d2);

        writer.commit();
    }

    IndexSearcher searcher(dir);

    // "title:python" — AND 模式：只有 doc 1 的 title 含 python
    auto r1 = searcher.search("title:python", 10, QueryMode::AND);
    if (r1.size() != 1)             FAIL("title:python AND should return 1 result");
    if (r1[0].doc_id != 1)          FAIL("title:python should match doc 1");

    // "python" (bare) — OR 模式：doc 1 (title) 和 doc 2 (body) 均命中
    auto r2 = searcher.search("python", 10, QueryMode::OR);
    if (r2.size() != 2)             FAIL("bare python OR should return 2 results");

    // "python" (bare) — AND 模式：bare term 展开到 title AND body，只有两个字段都有的 doc 命中
    // doc 1: title 有 python，body 无 → 不命中
    // doc 2: title 无 python，body 有 → 不命中
    auto r3 = searcher.search("python", 10, QueryMode::AND);
    if (r3.size() != 0)             FAIL("bare python AND should return 0 (strict: all fields must have term)");

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4：Merge 验证 — mergeAll() 后 per-field 文件保留，df 正确
// ─────────────────────────────────────────────────────────────────────────────
void test_merge_per_field() {
    TEST(merge_per_field);
    const std::string dir = tmpDir("merge");

    {
        // 写两个 Segment（ram 很小以强制两次 flush）
        IndexWriter writer(dir, 0.001f, makeSchema());

        Document d1; d1.doc_id = 1;
        d1.set("title", "python guide");
        d1.set("body",  "python programming");
        writer.addDocument(d1);
        writer.flush();

        Document d2; d2.doc_id = 2;
        d2.set("title", "java tutorial");
        d2.set("body",  "python java comparison");
        writer.addDocument(d2);
        writer.commit();
    }

    SegmentMerger merger(dir);
    auto stats = merger.mergeAll(99);

    if (stats.output_doc_count != 2) FAIL("merged segment should have 2 docs");

    // 新 Segment 的 per-field 文件必须存在（新路径：segment_99/）
    if (!fs::exists(dir + "/segment_99/tim_title")) FAIL("missing segment_99/tim_title after merge");
    if (!fs::exists(dir + "/segment_99/doc_title")) FAIL("missing segment_99/doc_title after merge");
    if (!fs::exists(dir + "/segment_99/tim_body"))  FAIL("missing segment_99/tim_body after merge");
    if (!fs::exists(dir + "/segment_99/doc_body"))  FAIL("missing segment_99/doc_body after merge");

    // df 验证
    SegmentReader seg(dir, 99);

    const TermMeta* title_meta = seg.getTermMeta("title", "python");
    if (!title_meta)               FAIL("title:python not found after merge");
    if (title_meta->doc_freq != 1) FAIL("title:python df should be 1 after merge");

    const TermMeta* body_meta = seg.getTermMeta("body", "python");
    if (!body_meta)                FAIL("body:python not found after merge");
    if (body_meta->doc_freq != 2)  FAIL("body:python df should be 2 after merge");

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5：Legacy 兼容 — deprecated single-arg API 仍能访问 per-field 数据
//         （per-field 模式下，legacy getTermMeta(term) 返回第一个命中字段的 meta）
// ─────────────────────────────────────────────────────────────────────────────
void test_legacy_compat() {
    TEST(legacy_compat);
    const std::string dir = tmpDir("legacy");

    {
        IndexWriter writer(dir, 100.f, makeSchema());
        Document d; d.doc_id = 1;
        d.set("title", "python tutorial");
        d.set("body",  "python data science");
        writer.addDocument(d);
        writer.commit();
    }

    SegmentReader seg(dir, 0);

    // deprecated single-arg API：在 per-field 模式下仍可访问，返回某字段的 meta
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const TermMeta* legacy_meta = seg.getTermMeta("python");
#pragma clang diagnostic pop

    if (!legacy_meta) FAIL("legacy getTermMeta(term) should find python in some field");

    // 新 API 必须对每个字段独立有效
    if (!seg.getTermMeta("title", "python")) FAIL("per-field title:python should exist");
    if (!seg.getTermMeta("body",  "python")) FAIL("per-field body:python should exist");

    // indexedFieldNames 应返回两个字段
    const auto& fields = seg.indexedFieldNames();
    if (fields.size() != 2) FAIL("should have 2 indexed fields");

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "═══════════════════════════════════\n";
    std::cout << "  Per-Field Index Tests\n";
    std::cout << "═══════════════════════════════════\n\n";

    test_write_per_field_files();
    test_field_isolation();
    test_field_query();
    test_merge_per_field();
    test_legacy_compat();

    std::cout << "\nAll tests PASSED!\n";
    return 0;
}
