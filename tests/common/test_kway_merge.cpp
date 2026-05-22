// tests/common/test_kway_merge.cpp
//
// KwayMerge 单元测试
// 覆盖：
//   1. 2 路已排序整数序列合并
//   2. K 路不等长序列合并
//   3. 含空序列的处理
//   4. 单序列直通
//   5. 所有序列为空
//   6. 降序 Compare（std::greater）

#include "common/kway_merge.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace ii;

#define TEST(name) do { std::cout << "[TEST] " #name "... " << std::flush; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << msg << "\n"; assert(false); } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// 辅助：从 vector 构造 source 函数
// ─────────────────────────────────────────────────────────────────────────────

static auto makeSource(std::vector<int> vec) {
    return [data = std::move(vec), idx = size_t{0}]() mutable -> std::optional<int> {
        if (idx < data.size()) return data[idx++];
        return std::nullopt;
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. 2 路合并
// ─────────────────────────────────────────────────────────────────────────────

void test_two_way() {
    TEST(two_way_merge);
    KwayMerge<int> m;
    m.addSource(makeSource({1, 3, 5, 7}));
    m.addSource(makeSource({2, 4, 6, 8}));
    m.init();

    std::vector<int> result;
    int v; size_t src;
    while (m.next(v, &src)) result.push_back(v);

    std::vector<int> expected = {1,2,3,4,5,6,7,8};
    if (result != expected) FAIL("result mismatch");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. K 路不等长
// ─────────────────────────────────────────────────────────────────────────────

void test_kway_unequal() {
    TEST(kway_unequal_lengths);
    KwayMerge<int> m;
    m.addSource(makeSource({1, 10, 100}));
    m.addSource(makeSource({2, 3}));
    m.addSource(makeSource({5, 20, 30, 40, 50}));
    m.addSource(makeSource({7}));
    m.init();

    std::vector<int> result;
    int v;
    while (m.next(v)) result.push_back(v);

    // 验证有序
    for (size_t i = 1; i < result.size(); ++i) {
        if (result[i] < result[i-1])
            FAIL("out of order at idx " + std::to_string(i));
    }
    // 验证总数
    if (result.size() != 11) FAIL("size=" + std::to_string(result.size()));
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. 含空序列
// ─────────────────────────────────────────────────────────────────────────────

void test_with_empty_sources() {
    TEST(handles_empty_sources);
    KwayMerge<int> m;
    m.addSource(makeSource({}));           // 空
    m.addSource(makeSource({2, 4, 6}));
    m.addSource(makeSource({}));           // 空
    m.addSource(makeSource({1, 3, 5}));
    m.init();

    std::vector<int> result;
    int v;
    while (m.next(v)) result.push_back(v);

    std::vector<int> expected = {1,2,3,4,5,6};
    if (result != expected) FAIL("result mismatch");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. 单序列直通
// ─────────────────────────────────────────────────────────────────────────────

void test_single_source() {
    TEST(single_source_passthrough);
    KwayMerge<int> m;
    m.addSource(makeSource({3, 1, 4, 1, 5}));  // 注意：source 内部不要求有序时合并结果也无序
    m.init();

    std::vector<int> result;
    int v;
    while (m.next(v)) result.push_back(v);

    std::vector<int> expected = {3,1,4,1,5};
    if (result != expected) FAIL("single source should passthrough as-is");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. 所有序列为空
// ─────────────────────────────────────────────────────────────────────────────

void test_all_empty() {
    TEST(all_sources_empty);
    KwayMerge<int> m;
    m.addSource(makeSource({}));
    m.addSource(makeSource({}));
    m.init();

    int v;
    assert(!m.next(v));
    assert(m.empty());
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. source_idx 正确追踪来源
// ─────────────────────────────────────────────────────────────────────────────

void test_source_tracking() {
    TEST(source_idx_tracking);
    KwayMerge<int> m;
    m.addSource(makeSource({1, 3}));   // source 0
    m.addSource(makeSource({2, 4}));   // source 1
    m.init();

    int v; size_t src;
    m.next(v, &src); assert(v == 1 && src == 0);
    m.next(v, &src); assert(v == 2 && src == 1);
    m.next(v, &src); assert(v == 3 && src == 0);
    m.next(v, &src); assert(v == 4 && src == 1);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. 降序（std::greater）
// ─────────────────────────────────────────────────────────────────────────────

void test_descending() {
    TEST(descending_compare);
    // std::greater<int>：大的优先
    KwayMerge<int, std::greater<int>> m;
    m.addSource(makeSource({9, 7, 5}));  // 已按降序排列的 source
    m.addSource(makeSource({8, 6, 4}));
    m.init();

    std::vector<int> result;
    int v;
    while (m.next(v)) result.push_back(v);

    std::vector<int> expected = {9,8,7,6,5,4};
    if (result != expected) FAIL("descending merge mismatch");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. 字符串合并
// ─────────────────────────────────────────────────────────────────────────────

void test_string_merge() {
    TEST(string_merge);
    auto makeSrcStr = [](std::vector<std::string> v) {
        return [data = std::move(v), idx = size_t{0}]() mutable
               -> std::optional<std::string> {
            if (idx < data.size()) return data[idx++];
            return std::nullopt;
        };
    };

    KwayMerge<std::string> m;
    m.addSource(makeSrcStr({"apple", "cherry", "fig"}));
    m.addSource(makeSrcStr({"banana", "date", "elderberry"}));
    m.init();

    std::vector<std::string> result;
    std::string s;
    while (m.next(s)) result.push_back(s);

    std::vector<std::string> expected = {"apple","banana","cherry","date","elderberry","fig"};
    if (result != expected) FAIL("string merge mismatch");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== KwayMerge Tests ===\n";
    test_two_way();
    test_kway_unequal();
    test_with_empty_sources();
    test_single_source();
    test_all_empty();
    test_source_tracking();
    test_descending();
    test_string_merge();
    std::cout << "All KwayMerge tests passed.\n";
    return 0;
}
