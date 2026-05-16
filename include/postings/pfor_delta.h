#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pfor_delta.h  —  PForDelta 整数压缩 / 解压
//
// 算法步骤：
//   压缩：
//     1. Delta 编码：将升序 doc_id 序列变为差值序列
//     2. 选位宽 b：覆盖 90% 以上 delta 值的最小位宽
//     3. 主数据区：每个 delta 用 b bit 紧凑存储，超出部分置 0 占位
//     4. 补丁区：记录异常值的 (位置, 真实值)
//
//   解压（反向执行）：
//     1. 读 Header → 得 b、size、exc_count
//     2. 读主数据区 → 解出 b-bit delta
//     3. 读补丁区  → 填入异常值
//     4. 前缀和   → 还原 doc_id
//
// Block 大小固定为 128 个 doc（对齐 SIMD 寄存器）。
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
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
    static constexpr int BLOCK_SIZE = 128;  // 每 Block 固定 128 doc

    // 压缩：将升序 doc_id 列表压缩为若干 Block 的字节序列
    // 同时输出每个 Block 对应的 SkipNode 元数据
    // max_tf_norm：该 term 在本 segment 中的最大 tf_norm（不含 IDF），
    //   存入 BlockHeader.max_score，查询期乘以 global_idf 得到真正 block UB
    static std::vector<uint8_t> compress(
        const std::vector<DocId>& doc_ids,
        std::vector<SkipNode>&    skip_nodes_out,
        float                     max_tf_norm = 0.45f  // 默认 tf=1 时的 tf_norm
    );

    // 解压：将字节序列还原为 doc_id 列表
    static std::vector<DocId> decompress(
        const uint8_t* data,
        size_t         data_len,
        uint32_t       total_docs
    );

    // 解压单个 Block（给定起始指针），返回消耗的字节数
    static size_t decompressBlock(
        const uint8_t*        ptr,
        std::vector<DocId>&   out,
        DocId                 base_doc_id = 0  // 累加基准
    );

    // 计算单个 Block 占用的字节数（不解压，直接从 Header 算）
    static size_t blockByteSize(const uint8_t* ptr);

private:
    // 选择位宽 b：能覆盖 ≥90% delta 值的最小 b
    static uint8_t chooseBitWidth(const std::vector<uint32_t>& deltas);

    // 将 values 按 b bit 紧凑打包到 out 中
    static void packBits(
        const std::vector<uint32_t>& values,
        uint8_t b,
        std::vector<uint8_t>& out
    );

    // 从 b-bit 紧凑数据解出 count 个整数
    static std::vector<uint32_t> unpackBits(
        const uint8_t* data,
        uint8_t b,
        uint32_t count
    );
};

} // namespace ii
