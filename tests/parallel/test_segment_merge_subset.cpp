// tests/parallel/test_segment_merge_subset.cpp
//
// Step 5：SegmentMerger 新构造函数（指定 seg_ids）单独验证
//
// 验证维度：
//   1. 指定 seg_ids 加载后 mergeAll() 产出正确 doc_count
//   2. 合并后 term df 等于各子 segment 之和
//   3. 临时 Segment 文件被清理（只剩 final_id 对应文件）
//   4. 不指定的 Segment 文件不受影响

#include "index/index_writer.h"
#include "index/segment_merger.h"
#include "index/segment_reader.h"
#include "field/schema.h"
#include "core/types.h"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ii;
namespace fs = std::filesystem;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << (msg) << "\n"; assert(false); } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// 工具
// ─────────────────────────────────────────────────────────────────────────────

static Schema makeSchema() {
    Schema s;
    s.fields = {
        {"body",     FieldType::Text,    IndexOption::FreqsPositions, true,  false, false, {}},
        {"category", FieldType::Keyword, IndexOption::FreqsOnly,      false, false, false, {}},
    };
    return s;
}

// 用 IndexWriter 写一个独立 Segment，返回 seg_id（IndexWriter 固定从 0 开始）
// 此函数每次用新目录，seg_id 始终为 0
static void writeSingleSegment(const std::string& dir,
                                const std::vector<Document>& docs)
{
    fs::create_directories(dir);
    IndexWriter writer(dir, 512.f, makeSchema());
    DocId id = 0;
    for (const auto& doc : docs) {
        Document d = doc;
        d.doc_id = ++id;
        writer.addDocument(d);
    }
    writer.commit();
}

static std::map<std::string, uint32_t> getTermDf(
    const SegmentReader& r, const std::string& field)
{
    std::map<std::string, uint32_t> m;
    const auto* dict = r.fieldTermDict(field);
    if (dict) for (const auto& [t, meta] : *dict) m[t] = meta.doc_freq;
    return m;
}

// 统计目录下存在 si 文件的 segment_N/ 子目录对应的 seg_id。
// 已修复 bug（勿回退）：原实现扫描 _N.si 根目录文件，但实际格式为
// segment_N/si（无扩展名）。
static std::set<uint32_t> listSiIds(const std::string& dir) {
    std::set<uint32_t> ids;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_directory()) continue;
        const auto name = entry.path().filename().string();
        if (name.size() > 8 && name.substr(0, 8) == "segment_") {
            if (fs::exists(entry.path() / "si")) {
                try {
                    uint32_t id = static_cast<uint32_t>(std::stoul(name.substr(8)));
                    ids.insert(id);
                } catch (...) {}
            }
        }
    }
    return ids;
}

// ─────────────────────────────────────────────────────────────────────────────
// 构建共用测试数据：3 组文档，分别写入 3 个独立 Segment（seg_id 均为 0）
// 合并测试需要将它们放到同一目录，用不同 seg_id
// 策略：用三个临时子目录分别写出，再把文件复制/重命名到合并目录
// ─────────────────────────────────────────────────────────────────────────────

// 已修复 bug（勿回退）：IndexWriter 将文件写入 segment_0/ 子目录（非 _0.* 根目录文件）。
// 原实现在 src_dir 根目录寻找 _0.* 文件，匹配失败 → dst_dir 空目录 →
// SegmentMerger 抛 "Cannot open .tim: .../merged/segment_0/tim"。
// 修复：复制 src_dir/segment_0/ 整个目录到 dst_dir/segment_<new_id>/。
static void copySegmentAs(const std::string& src_dir,
                           const std::string& dst_dir,
                           uint32_t           new_id)
{
    std::string src_seg = src_dir + "/segment_0";
    std::string dst_seg = dst_dir + "/segment_" + std::to_string(new_id);
    fs::create_directories(dst_seg);
    for (const auto& entry : fs::directory_iterator(src_seg)) {
        fs::copy_file(entry.path(),
                      fs::path(dst_seg) / entry.path().filename(),
                      fs::copy_options::overwrite_existing);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 生成文档组
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<Document> makeGroup(const std::vector<std::string>& bodies,
                                        const std::string& cat)
{
    std::vector<Document> docs;
    for (const auto& b : bodies) {
        Document d;
        d.set("body", b);
        d.set("category", cat);
        docs.push_back(std::move(d));
    }
    return docs;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1：指定 seg_ids 加载后 mergeAll() 产出正确 doc_count
// ─────────────────────────────────────────────────────────────────────────────

void test_merge_doc_count() {
    TEST(merge_subset_doc_count_correct);

    const std::string base  = "/tmp/piggy_ms_t1";
    const std::string tmp0  = base + "/tmp0";
    const std::string tmp1  = base + "/tmp1";
    const std::string tmp2  = base + "/tmp2";
    const std::string merge_dir = base + "/merged";
    fs::remove_all(base);

    auto g0 = makeGroup({"search engine inverted index", "bm25 ranking term frequency"}, "search");
    auto g1 = makeGroup({"machine learning gradient descent", "neural network backprop"}, "ml");
    auto g2 = makeGroup({"database btree hash index", "query execution plan optimizer", "join algorithm"}, "db");

    writeSingleSegment(tmp0, g0);
    writeSingleSegment(tmp1, g1);
    writeSingleSegment(tmp2, g2);

    // 把 3 个 seg_id=0 的文件复制为 seg_id 0,1,2 到 merge_dir
    fs::create_directories(merge_dir);
    makeSchema().save(merge_dir);
    copySegmentAs(tmp0, merge_dir, 0);
    copySegmentAs(tmp1, merge_dir, 1);
    copySegmentAs(tmp2, merge_dir, 2);

    constexpr uint32_t FINAL_ID = 10;
    SegmentMerger merger(merge_dir, {0, 1, 2});
    merger.mergeAll(FINAL_ID);

    SegmentReader reader(merge_dir, FINAL_ID);
    uint32_t expected = static_cast<uint32_t>(g0.size() + g1.size() + g2.size());
    if (reader.docCount() != expected)
        FAIL("docCount=" + std::to_string(reader.docCount())
             + " expected=" + std::to_string(expected));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2：合并后 term df 等于各子 Segment 中相同 term 的 df 之和
// ─────────────────────────────────────────────────────────────────────────────

void test_merge_term_df_sum() {
    TEST(merge_subset_term_df_is_sum_of_parts);

    const std::string base  = "/tmp/piggy_ms_t2";
    const std::string tmp0  = base + "/tmp0";
    const std::string tmp1  = base + "/tmp1";
    const std::string merge_dir = base + "/merged";
    fs::remove_all(base);

    // 两组文档共享词 "index"
    auto g0 = makeGroup({"search engine inverted index ranking"}, "search");
    auto g1 = makeGroup({"database index btree hash", "secondary index scan"}, "db");

    writeSingleSegment(tmp0, g0);
    writeSingleSegment(tmp1, g1);

    fs::create_directories(merge_dir);
    makeSchema().save(merge_dir);
    copySegmentAs(tmp0, merge_dir, 0);
    copySegmentAs(tmp1, merge_dir, 1);

    // 记录合并前各 segment 的 df
    SegmentReader r0(merge_dir, 0), r1(merge_dir, 1);
    auto df0 = getTermDf(r0, "body");
    auto df1 = getTermDf(r1, "body");

    constexpr uint32_t FINAL_ID = 5;
    SegmentMerger merger(merge_dir, {0, 1});
    merger.mergeAll(FINAL_ID);

    SegmentReader rf(merge_dir, FINAL_ID);
    auto dfm = getTermDf(rf, "body");

    // 验证 "index" 的 df = df0["index"] + df1["index"]
    const std::string check_term = "index";
    uint32_t sum = (df0.count(check_term) ? df0[check_term] : 0)
                 + (df1.count(check_term) ? df1[check_term] : 0);
    if (sum == 0) FAIL("term 'index' not found in either segment");
    auto it = dfm.find(check_term);
    if (it == dfm.end())
        FAIL("term 'index' missing from merged segment");
    if (it->second != sum)
        FAIL("merged df=" + std::to_string(it->second) + " expected=" + std::to_string(sum));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3：合并后临时 Segment 文件被清理，只剩 FINAL_ID 对应 .si
// ─────────────────────────────────────────────────────────────────────────────

void test_merge_cleans_temp_files() {
    TEST(merge_subset_cleans_temp_segment_files);

    const std::string base  = "/tmp/piggy_ms_t3";
    const std::string tmp0  = base + "/tmp0";
    const std::string tmp1  = base + "/tmp1";
    const std::string merge_dir = base + "/merged";
    fs::remove_all(base);

    auto g0 = makeGroup({"alpha beta gamma"}, "cat0");
    auto g1 = makeGroup({"delta epsilon zeta"}, "cat1");

    writeSingleSegment(tmp0, g0);
    writeSingleSegment(tmp1, g1);

    fs::create_directories(merge_dir);
    makeSchema().save(merge_dir);
    copySegmentAs(tmp0, merge_dir, 0);
    copySegmentAs(tmp1, merge_dir, 1);

    constexpr uint32_t FINAL_ID = 7;
    SegmentMerger merger(merge_dir, {0, 1});
    merger.mergeAll(FINAL_ID);

    auto si_ids = listSiIds(merge_dir);
    if (si_ids.size() != 1)
        FAIL("expected 1 .si file, found " + std::to_string(si_ids.size()));
    if (*si_ids.begin() != FINAL_ID)
        FAIL(".si id=" + std::to_string(*si_ids.begin())
             + " expected=" + std::to_string(FINAL_ID));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4：不在 seg_ids 列表中的 Segment 文件不受影响
// ─────────────────────────────────────────────────────────────────────────────

void test_merge_leaves_other_segments_intact() {
    TEST(merge_subset_leaves_unspecified_segments_intact);

    const std::string base  = "/tmp/piggy_ms_t4";
    const std::string tmp0  = base + "/tmp0";
    const std::string tmp1  = base + "/tmp1";
    const std::string tmp2  = base + "/tmp2";
    const std::string merge_dir = base + "/merged";
    fs::remove_all(base);

    auto g0 = makeGroup({"alpha beta"}, "cat0");
    auto g1 = makeGroup({"gamma delta"}, "cat1");
    auto g2 = makeGroup({"epsilon zeta"}, "cat2");  // NOT merged

    writeSingleSegment(tmp0, g0);
    writeSingleSegment(tmp1, g1);
    writeSingleSegment(tmp2, g2);

    fs::create_directories(merge_dir);
    makeSchema().save(merge_dir);
    copySegmentAs(tmp0, merge_dir, 0);
    copySegmentAs(tmp1, merge_dir, 1);
    copySegmentAs(tmp2, merge_dir, 2);  // seg_id=2, NOT in merge list

    constexpr uint32_t FINAL_ID = 9;
    SegmentMerger merger(merge_dir, {0, 1});  // only merge 0,1
    merger.mergeAll(FINAL_ID);

    // seg_id=2 的 .si 文件应该还在
    auto si_ids = listSiIds(merge_dir);
    if (si_ids.find(2) == si_ids.end())
        FAIL("seg_id=2 .si file was unexpectedly deleted");
    if (si_ids.find(FINAL_ID) == si_ids.end())
        FAIL("final seg .si file not found");

    // seg_id=0 和 1 的 .si 应该已被删除
    if (si_ids.find(0) != si_ids.end())
        FAIL("seg_id=0 .si should have been deleted");
    if (si_ids.find(1) != si_ids.end())
        FAIL("seg_id=1 .si should have been deleted");

    // seg_id=2 的数据可读
    SegmentReader r2(merge_dir, 2);
    if (r2.docCount() != static_cast<uint32_t>(g2.size()))
        FAIL("seg_id=2 docCount=" + std::to_string(r2.docCount()));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== SegmentMerger Subset Tests (Step 5) ===\n\n";

    test_merge_doc_count();
    test_merge_term_df_sum();
    test_merge_cleans_temp_files();
    test_merge_leaves_other_segments_intact();

    std::cout << "\nAll Step 5 tests passed.\n";
    return 0;
}
