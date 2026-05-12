#include "skiplist.h"
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// 构造：从 SkipNode 数组建 Level0，按需建 Level1
// ─────────────────────────────────────────────────────────────────────────────

SkipList::SkipList(std::vector<SkipNode> nodes)
    : level0_(std::move(nodes))
{
    buildLevel1();
}

void SkipList::buildLevel1() {
    // 只有 Level0 节点数超过 SKIP_INTERVAL 才建 Level1
    if (level0_.size() <= (size_t)SKIP_INTERVAL) return;

    level1_.clear();
    for (size_t i = 0; i < level0_.size(); i += SKIP_INTERVAL) {
        // Level1 节点代表从 i 开始的 SKIP_INTERVAL 个 Level0 节点
        // max_doc_id 取这批中最后一个的 max_doc_id
        size_t last = std::min(i + SKIP_INTERVAL - 1, level0_.size() - 1);
        SkipNode sn;
        sn.max_doc_id  = level0_[last].max_doc_id;
        sn.byte_offset = level0_[i].byte_offset;  // 该批第一个 Block 的偏移
        sn.max_score   = 0.0f;
        for (size_t j = i; j <= last; ++j) {
            sn.max_score = std::max(sn.max_score, level0_[j].max_score);
        }
        sn.doc_count = 0;  // Level1 不用 doc_count
        level1_.push_back(sn);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// find：返回包含 target_doc_id 的 Block 的字节偏移
// ─────────────────────────────────────────────────────────────────────────────

SkipList::FindResult SkipList::find(DocId target_doc_id) const {
    if (level0_.empty()) {
        return {UINT64_MAX, SIZE_MAX, 0};
    }

    // Step1：用 Level1 快速定位 Level0 的起始搜索范围
    size_t l0_start = 0;
    if (!level1_.empty()) {
        // 找第一个 max_doc_id >= target 的 Level1 节点
        for (size_t i = 0; i < level1_.size(); ++i) {
            if (level1_[i].max_doc_id >= target_doc_id) {
                // Level0 的搜索范围从 i*SKIP_INTERVAL 开始
                l0_start = i * SKIP_INTERVAL;
                break;
            }
            // 如果所有 Level1 节点 max_doc_id 都 < target，target 超出范围
            if (i == level1_.size() - 1) {
                return {UINT64_MAX, SIZE_MAX, 0};
            }
        }
    }

    // Step2：在 Level0 中线性扫描（范围已被 Level1 缩小）
    for (size_t i = l0_start; i < level0_.size(); ++i) {
        if (level0_[i].max_doc_id >= target_doc_id) {
            // 找到！返回该 Block 的字节偏移
            // first_doc_id 需要从 Header 读，这里先传 0，由调用方处理
            return {level0_[i].byte_offset, i, 0};
        }
    }
    return {UINT64_MAX, SIZE_MAX, 0};  // target 超出所有 Block
}

// ─────────────────────────────────────────────────────────────────────────────
// 序列化格式：
//   4B  level0_count
//   4B  level1_count
//   level0_count × sizeof(SkipNode)
//   level1_count × sizeof(SkipNode)
// ─────────────────────────────────────────────────────────────────────────────

size_t SkipList::serializedSize() const {
    return 8
         + level0_.size() * sizeof(SkipNode)
         + level1_.size() * sizeof(SkipNode);
}

std::vector<uint8_t> SkipList::serialize() const {
    std::vector<uint8_t> buf(serializedSize());
    uint8_t* p = buf.data();

    auto write32 = [&](uint32_t v) {
        std::memcpy(p, &v, 4); p += 4;
    };
    write32(static_cast<uint32_t>(level0_.size()));
    write32(static_cast<uint32_t>(level1_.size()));

    if (!level0_.empty()) {
        std::memcpy(p, level0_.data(), level0_.size() * sizeof(SkipNode));
        p += level0_.size() * sizeof(SkipNode);
    }
    if (!level1_.empty()) {
        std::memcpy(p, level1_.data(), level1_.size() * sizeof(SkipNode));
    }
    return buf;
}

SkipList SkipList::deserialize(const uint8_t* data, size_t len) {
    if (len < 8) throw std::runtime_error("SkipList data too short");

    uint32_t l0_count, l1_count;
    std::memcpy(&l0_count, data,     4);
    std::memcpy(&l1_count, data + 4, 4);

    const uint8_t* p = data + 8;

    std::vector<SkipNode> l0(l0_count);
    if (l0_count > 0) {
        std::memcpy(l0.data(), p, l0_count * sizeof(SkipNode));
        p += l0_count * sizeof(SkipNode);
    }

    std::vector<SkipNode> l1(l1_count);
    if (l1_count > 0) {
        std::memcpy(l1.data(), p, l1_count * sizeof(SkipNode));
    }

    SkipList sl;
    sl.level0_ = std::move(l0);
    sl.level1_ = std::move(l1);
    return sl;
}

} // namespace ii
