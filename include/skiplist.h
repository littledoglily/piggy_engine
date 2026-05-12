#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// skiplist.h  —  Block 级跳表
//
// 结构：
//   Level0：每 128 个 doc 一个节点（每 Block 一个节点）
//   Level1：当 Level0 节点数 > 128 时自动建立（节点间隔 128 个 Block）
//
// 每个节点存储：
//   max_doc_id   —— 用于判断目标 doc 是否在本 Block
//   byte_offset  —— Block 在文件中的字节偏移（因 PForDelta 大小可变）
//   max_score    —— BMW/WAND 块级分数上界
//   doc_count    —— 本 Block 实际 doc 数
//
// 查找接口：
//   给定 target_doc_id，返回最近不超过 target 的 Block 的 byte_offset
//   时间复杂度 O(log N)（多级跳表）
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include <vector>
#include <cstdint>

namespace ii {

class SkipList {
public:
    static constexpr int SKIP_INTERVAL = 128;  // 每 128 个 Level0 节点建一个 Level1

    // 从 SkipNode 数组构建跳表（flush 时调用）
    explicit SkipList(std::vector<SkipNode> nodes);

    // 默认构造（空跳表）
    SkipList() = default;

    // 序列化到字节流（写入 .doc 文件）
    std::vector<uint8_t> serialize() const;

    // 从字节流反序列化（读取 .doc 文件时）
    static SkipList deserialize(const uint8_t* data, size_t len);

    // 核心查询：找到包含 target_doc_id 的 Block 的字节偏移
    // 返回值：{byte_offset, block_index}
    // 若 target 超出所有 Block，返回 {UINT64_MAX, SIZE_MAX}
    struct FindResult {
        uint64_t byte_offset;
        size_t   block_index;
        DocId    block_first_doc;  // 该 Block 的 first_doc_id（用于解压基准）
    };
    FindResult find(DocId target_doc_id) const;

    // 获取第 i 个 Level0 节点
    const SkipNode& node(size_t i) const { return level0_[i]; }
    size_t          size()         const { return level0_.size(); }
    bool            empty()        const { return level0_.empty(); }

    // 序列化大小（字节数）
    size_t serializedSize() const;

private:
    std::vector<SkipNode> level0_;  // 每个 Block 一个节点
    std::vector<SkipNode> level1_;  // 每 128 个 Block 一个节点（可选）

    void buildLevel1();
};

} // namespace ii
