#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// codec/pfor_delta.h  —  PForDelta 整数压缩 / 解压
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include <cstdint>
#include <vector>

namespace ii {

// ── Block 物理 Header（16 byte 固定大小）─────────────────────────────────────
#pragma pack(push, 1)
struct BlockHeader {
    uint8_t  b;             // 位宽（1~32）
    uint8_t  size;          // 本 Block 实际 doc 数（1~128）
    uint8_t  exc_count;     // 异常值数量
    uint8_t  reserved;      // 对齐保留
    uint32_t max_doc_id;    // 本 Block 最大 doc_id
    float    max_score;     // 本 Block 最高 BM25 贡献分（BMW 用）
    uint32_t first_doc_id;  // 本 Block 第一个 doc_id（用于跳跃恢复）
};
static_assert(sizeof(BlockHeader) == 16, "BlockHeader must be 16 bytes");
#pragma pack(pop)

// ── 补丁记录（每个 6 byte）───────────────────────────────────────────────────
#pragma pack(push, 1)
struct PatchEntry {
    uint16_t position;  // 在 128 个 delta 中的下标
    uint32_t value;     // 真实 delta 值
};
static_assert(sizeof(PatchEntry) == 6, "PatchEntry must be 6 bytes");
#pragma pack(pop)

// ── PForDelta 编解码器 ────────────────────────────────────────────────────────
class PForDelta {
public:
    static constexpr int BLOCK_SIZE = 128;

    static std::vector<uint8_t> compress(
        const std::vector<DocId>&   doc_ids,
        std::vector<SkipNode>&      skip_nodes_out,
        const std::vector<float>&   block_max_tf_norms = {}
    );

    static std::vector<DocId> decompress(
        const uint8_t* data,
        size_t         data_len,
        uint32_t       total_docs
    );

    static size_t decompressBlock(
        const uint8_t*        ptr,
        std::vector<DocId>&   out,
        DocId                 base_doc_id = 0
    );

    static size_t blockByteSize(const uint8_t* ptr);

private:
    static uint8_t chooseBitWidth(const std::vector<uint32_t>& deltas);
    static void packBits(const std::vector<uint32_t>& values, uint8_t b, std::vector<uint8_t>& out);
    static std::vector<uint32_t> unpackBits(const uint8_t* data, uint8_t b, uint32_t count);
};

} // namespace ii
