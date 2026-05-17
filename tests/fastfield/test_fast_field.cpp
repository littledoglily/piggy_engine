// tests/fastfield/test_fast_field.cpp
// FastField 列存读写、范围过滤、IndexWriter 集成测试
#include "../../tests/test_utils.h"
#include "fastfield/fast_field_writer.h"
#include "fastfield/fast_field_reader.h"
#include "core/index_writer.h"
#include "query/index_searcher.h"
#include "types.h"

#include <filesystem>
#include <cassert>
#include <cstdlib>
#include <string>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：临时目录（测试结束后清理）
// ─────────────────────────────────────────────────────────────────────────────
static std::string tmpDir(const std::string& suffix) {
    std::string dir = "/tmp/test_ff_" + suffix;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1: FastFieldWriter / FastFieldReader 基本读写
// ─────────────────────────────────────────────────────────────────────────────
void test_writer_reader_roundtrip() {
    TEST(writer_reader_roundtrip);

    auto dir = tmpDir("rw");
    const uint32_t SEG = 0;
    const uint32_t N   = 5;

    // 写
    ii::FastFieldWriter w;
    for (uint32_t i = 0; i < N; ++i) {
        ii::FastFieldDoc d;
        d.pubtime   = 1000LL + i;
        d.uid       = 100LL  + i;
        d.page_rank = 0.1f * (float)i;
        w.add(d);
    }
    w.flush(dir, SEG);

    // 读
    ii::FastFieldReader r(dir, SEG, N);
    if (!r.hasData())                 FAIL("no data loaded");
    for (uint32_t i = 0; i < N; ++i) {
        if (r.pubtime(i)   != 1000LL + i)    FAIL("pubtime mismatch at " + std::to_string(i));
        if (r.uid(i)       != 100LL  + i)    FAIL("uid mismatch at " + std::to_string(i));
        float expected = 0.1f * (float)i;
        float diff = r.pageRank(i) - expected;
        if (diff < -1e-5f || diff > 1e-5f)   FAIL("page_rank mismatch at " + std::to_string(i));
    }

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: filterPubtime 范围过滤
// ─────────────────────────────────────────────────────────────────────────────
void test_filter_pubtime() {
    TEST(filter_pubtime);

    auto dir = tmpDir("fp");
    const uint32_t SEG = 0;

    ii::FastFieldWriter w;
    // pubtime: 1000, 1001, 1002, 1003, 1004
    for (uint32_t i = 0; i < 5; ++i) {
        ii::FastFieldDoc d;
        d.pubtime   = 1000LL + i;
        d.uid       = 0;
        d.page_rank = 0.0f;
        w.add(d);
    }
    w.flush(dir, SEG);

    ii::FastFieldReader r(dir, SEG, 5);

    // [1001, 1003] → indices 1, 2, 3
    auto res = r.filterPubtime(1001LL, 1003LL);
    if (res.size() != 3)              FAIL("expected 3 results, got " + std::to_string(res.size()));
    if (res[0] != 1 || res[1] != 2 || res[2] != 3) FAIL("wrong indices");

    // exact match
    auto res2 = r.filterPubtime(1002LL, 1002LL);
    if (res2.size() != 1 || res2[0] != 2) FAIL("exact match failed");

    // empty range
    auto res3 = r.filterPubtime(2000LL, 3000LL);
    if (!res3.empty())                FAIL("expected empty, got " + std::to_string(res3.size()));

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: filterUid 精确匹配
// ─────────────────────────────────────────────────────────────────────────────
void test_filter_uid() {
    TEST(filter_uid);

    auto dir = tmpDir("uid");
    const uint32_t SEG = 0;

    ii::FastFieldWriter w;
    // uid: 10, 20, 10, 30, 10
    int64_t uids[] = {10, 20, 10, 30, 10};
    for (int i = 0; i < 5; ++i) {
        ii::FastFieldDoc d;
        d.pubtime   = 0;
        d.uid       = uids[i];
        d.page_rank = 0.0f;
        w.add(d);
    }
    w.flush(dir, SEG);

    ii::FastFieldReader r(dir, SEG, 5);

    auto res = r.filterUid(10LL);
    if (res.size() != 3)              FAIL("expected 3 uid=10, got " + std::to_string(res.size()));
    if (res[0]!=0 || res[1]!=2 || res[2]!=4) FAIL("wrong uid indices");

    auto res2 = r.filterUid(99LL);
    if (!res2.empty())                FAIL("uid=99 should be empty");

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: 旧 Segment（无 FF 文件）reader 静默跳过
// ─────────────────────────────────────────────────────────────────────────────
void test_missing_ff_files() {
    TEST(missing_ff_files);

    auto dir = tmpDir("missing");
    // 不写任何 FF 文件
    ii::FastFieldReader r(dir, 0, 3);
    if (r.hasData())                  FAIL("should have no data for missing files");
    if (r.pubtime(0) != 0)            FAIL("pubtime should return 0 for missing");
    if (r.filterPubtime(0, 9999).size() != 0) FAIL("filter should be empty for missing");

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: IndexWriter 集成 — FF 随 addDocument 写入并可读取
// ─────────────────────────────────────────────────────────────────────────────
void test_index_writer_integration() {
    TEST(index_writer_integration);

    auto dir = tmpDir("iw");

    {
        ii::IndexWriter iw(dir, 1.0f);
        ii::Document d;
        d.doc_id    = 1;
        d.set("title", "apple fruit");
        d.set("body", "apple is a fruit");
        d.set("category", "food");
        d.set("pubtime", 20240101LL);
        d.set("uid", 42LL);
        d.set("page_rank", 0.8f);
        iw.addDocument(d);

        d.doc_id    = 2;
        d.set("title", "banana fruit");
        d.set("body", "banana is yellow");
        d.set("pubtime", 20240201LL);
        d.set("uid", 99LL);
        d.set("page_rank", 0.5f);
        iw.addDocument(d);

        iw.commit();
    }

    // 用 SegmentReader 打开检查
    ii::SegmentReader sr(dir, 0);
    if (!sr.hasFastField())           FAIL("segment should have FastField");

    // doc_id=1 → local_idx=0
    if (sr.ffPubtime(0) != 20240101LL) FAIL("pubtime mismatch doc1");
    if (sr.ffUid(0)     != 42LL)       FAIL("uid mismatch doc1");

    if (sr.ffPubtime(1) != 20240201LL) FAIL("pubtime mismatch doc2");
    if (sr.ffUid(1)     != 99LL)       FAIL("uid mismatch doc2");

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6: IndexSearcher 带 NumericFilter 搜索
// ─────────────────────────────────────────────────────────────────────────────
void test_searcher_with_filter() {
    TEST(searcher_with_filter);

    auto dir = tmpDir("srch");

    {
        ii::IndexWriter iw(dir, 1.0f);
        for (int i = 1; i <= 5; ++i) {
            ii::Document d;
            d.doc_id    = (ii::DocId)i;
            d.set("title", "fruit food");
            d.set("body", "fruit is good food");
            d.set("pubtime", 20240100LL + i);   // 20240101 ~ 20240105
            d.set("uid", (int64_t)(i * 10));
            d.set("page_rank", 0.1f * (float)i);
            iw.addDocument(d);
        }
        iw.commit();
    }

    ii::IndexSearcher searcher(dir);

    // pubtime 范围 [20240102, 20240104] → doc 2,3,4
    ii::NumericFilter f;
    f.pubtime_lo = 20240102LL;
    f.pubtime_hi = 20240104LL;

    auto results = searcher.search("fruit", f, 10, ii::QueryMode::OR);
    if (results.size() != 3)          FAIL("expected 3 results with pubtime filter, got " + std::to_string(results.size()));

    // uid 精确匹配 uid=30 → doc 3
    ii::NumericFilter f2;
    f2.uid = 30LL;
    auto r2 = searcher.search("fruit", f2, 10, ii::QueryMode::OR);
    if (r2.size() != 1)               FAIL("expected 1 result with uid filter, got " + std::to_string(r2.size()));
    if (r2[0].uid != 30LL)            FAIL("wrong uid in result");

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "=== FastField Tests ===\n";
    test_writer_reader_roundtrip();
    test_filter_pubtime();
    test_filter_uid();
    test_missing_ff_files();
    test_index_writer_integration();
    test_searcher_with_filter();
    std::cout << "All FastField tests passed.\n";
    return 0;
}
