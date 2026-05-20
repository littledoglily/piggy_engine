#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// codec/skiplist.h  —  Block 级两级跳表
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include <vector>
#include <cstdint>

namespace ii {

class SkipList {
public:
    static constexpr int SKIP_INTERVAL = 128;

    explicit SkipList(std::vector<SkipNode> nodes);
    SkipList() = default;

    std::vector<uint8_t> serialize() const;
    static SkipList deserialize(const uint8_t* data, size_t len);

    struct FindResult {
        uint64_t byte_offset;
        size_t   block_index;
        DocId    block_first_doc;
    };
    FindResult find(DocId target_doc_id) const;

    const SkipNode& node(size_t i) const { return level0_[i]; }
    size_t          size()         const { return level0_.size(); }
    bool            empty()        const { return level0_.empty(); }
    size_t          serializedSize() const;

private:
    std::vector<SkipNode> level0_;
    std::vector<SkipNode> level1_;
    void buildLevel1();
};

} // namespace ii
