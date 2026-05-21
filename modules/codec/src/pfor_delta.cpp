#include "codec/pfor_delta.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// 工具函数：b-bit 紧凑打包 / 解包
// ─────────────────────────────────────────────────────────────────────────────

void PForDelta::packBits(const std::vector<uint32_t>& values,
                         uint8_t b,
                         std::vector<uint8_t>& out)
{
    if (b == 0 || values.empty()) return;

    // 计算所需字节数：ceil(count * b / 8)
    size_t total_bits = values.size() * b;
    size_t total_bytes = (total_bits + 7) / 8;
    size_t base = out.size();
    out.resize(base + total_bytes, 0);

    uint64_t bit_pos = 0;
    for (uint32_t v : values) {
        // 将 v 的低 b bit 写入 out，从 bit_pos 开始
        for (uint8_t i = 0; i < b; ++i) {
            if ((v >> i) & 1u) {
                size_t byte_idx = base + (bit_pos + i) / 8;
                uint8_t  bit_idx = (bit_pos + i) % 8;
                out[byte_idx] |= (1u << bit_idx);
            }
        }
        bit_pos += b;
    }
}

std::vector<uint32_t> PForDelta::unpackBits(const uint8_t* data,
                                             uint8_t b,
                                             uint32_t count)
{
    std::vector<uint32_t> result(count, 0);
    if (b == 0) return result;  // 全 0

    uint64_t bit_pos = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = 0;
        for (uint8_t j = 0; j < b; ++j) {
            size_t byte_idx = (bit_pos + j) / 8;
            uint8_t bit_idx = (bit_pos + j) % 8;
            if ((data[byte_idx] >> bit_idx) & 1u) {
                v |= (1u << j);
            }
        }
        result[i] = v;
        bit_pos += b;
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 选位宽：找能覆盖 ≥90% delta 值的最小 b
// ─────────────────────────────────────────────────────────────────────────────

uint8_t PForDelta::chooseBitWidth(const std::vector<uint32_t>& deltas) {
    if (deltas.empty()) return 1;

    size_t n = deltas.size();
    size_t threshold = (n * 9 + 9) / 10;  // ceil(90%)

    for (uint8_t b = 1; b <= 32; ++b) {
        uint32_t max_val = (b == 32) ? 0xFFFFFFFFu : ((1u << b) - 1u);
        size_t covered = 0;
        for (uint32_t d : deltas) {
            if (d <= max_val) ++covered;
        }
        if (covered >= threshold) return b;
    }
    return 32;
}

// ─────────────────────────────────────────────────────────────────────────────
// 压缩主流程
// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> PForDelta::compress(
    const std::vector<DocId>&   doc_ids,
    std::vector<SkipNode>&      skip_nodes_out,
    const std::vector<float>&   block_max_tf_norms)
{
    std::vector<uint8_t> output;
    skip_nodes_out.clear();

    size_t i = 0;
    uint64_t block_data_offset = 0;  // 相对 output 起点的字节偏移

    while (i < doc_ids.size()) {
        // 取当前 Block 的 doc_id 列表（最多 128 个）
        size_t block_end = std::min(i + (size_t)BLOCK_SIZE, doc_ids.size());
        std::vector<DocId> block_docs(doc_ids.begin() + i, doc_ids.begin() + block_end);

        // Step1：Delta 编码（首个 delta = doc_id 本身）
        std::vector<uint32_t> deltas(block_docs.size());
        deltas[0] = block_docs[0];
        for (size_t k = 1; k < block_docs.size(); ++k) {
            deltas[k] = block_docs[k] - block_docs[k-1];
        }

        // Step2：选位宽 b
        uint8_t b = chooseBitWidth(deltas);
        uint32_t max_val = (b == 32) ? 0xFFFFFFFFu : ((1u << b) - 1u);

        // Step3：分离正常值（truncate to b bit）和异常值
        std::vector<uint32_t> truncated(deltas.size());
        std::vector<PatchEntry> patches;

        for (size_t k = 0; k < deltas.size(); ++k) {
            if (deltas[k] <= max_val) {
                truncated[k] = deltas[k];
            } else {
                truncated[k] = 0;  // 占位
                PatchEntry pe;
                pe.position = static_cast<uint16_t>(k);
                pe.value    = deltas[k];
                patches.push_back(pe);
            }
        }

        // Step4：构建 BlockHeader
        BlockHeader hdr;
        hdr.b           = b;
        hdr.size        = static_cast<uint8_t>(block_docs.size());
        hdr.exc_count   = static_cast<uint8_t>(patches.size());
        hdr.reserved    = 0;
        hdr.max_doc_id  = block_docs.back();
        hdr.first_doc_id= block_docs.front();
        // max_score 存本 Block 的 max_tf_norm（不含 IDF），查询期 × global_idf 得 block UB
        size_t blk_idx  = skip_nodes_out.size();  // 当前 Block 索引（push 前等于已有数量）
        hdr.max_score   = (blk_idx < block_max_tf_norms.size())
                          ? block_max_tf_norms[blk_idx]
                          : 0.0f;

        // Step5：写 Header
        size_t block_start_offset = output.size();
        output.resize(output.size() + sizeof(BlockHeader));
        std::memcpy(output.data() + block_start_offset, &hdr, sizeof(BlockHeader));

        // Step6：写主数据区（b-bit 紧凑）
        packBits(truncated, b, output);

        // Step7：写补丁区
        for (const auto& pe : patches) {
            size_t off = output.size();
            output.resize(off + sizeof(PatchEntry));
            std::memcpy(output.data() + off, &pe, sizeof(PatchEntry));
        }

        // Step8：记录 SkipNode
        SkipNode sn;
        sn.max_doc_id  = hdr.max_doc_id;
        sn.byte_offset = static_cast<uint64_t>(block_start_offset);
        sn.max_score   = hdr.max_score;
        sn.doc_count   = hdr.size;
        skip_nodes_out.push_back(sn);

        i = block_end;
    }
    return output;
}

// ─────────────────────────────────────────────────────────────────────────────
// 计算单个 Block 的字节大小（无需解压）
// ─────────────────────────────────────────────────────────────────────────────

size_t PForDelta::blockByteSize(const uint8_t* ptr) {
    const BlockHeader* hdr = reinterpret_cast<const BlockHeader*>(ptr);
    size_t main_bits  = (size_t)hdr->size * hdr->b;
    size_t main_bytes = (main_bits + 7) / 8;
    size_t patch_bytes= (size_t)hdr->exc_count * sizeof(PatchEntry);
    return sizeof(BlockHeader) + main_bytes + patch_bytes;
}

// ─────────────────────────────────────────────────────────────────────────────
// 解压单个 Block
// ─────────────────────────────────────────────────────────────────────────────

size_t PForDelta::decompressBlock(const uint8_t*      ptr,
                                   std::vector<DocId>& out,
                                   DocId               base_doc_id)
{
    const BlockHeader* hdr = reinterpret_cast<const BlockHeader*>(ptr);
    const uint8_t* main_data = ptr + sizeof(BlockHeader);

    // 解压主数据区
    std::vector<uint32_t> deltas = unpackBits(main_data, hdr->b, hdr->size);

    // 主数据区字节数
    size_t main_bits  = (size_t)hdr->size * hdr->b;
    size_t main_bytes = (main_bits + 7) / 8;

    // 读补丁区，填入异常值
    const uint8_t* patch_data = main_data + main_bytes;
    for (uint8_t p = 0; p < hdr->exc_count; ++p) {
        PatchEntry pe;
        std::memcpy(&pe, patch_data + p * sizeof(PatchEntry), sizeof(PatchEntry));
        deltas[pe.position] = pe.value;
    }

    // 前缀和还原 doc_id
    DocId prev = base_doc_id;
    for (uint32_t k = 0; k < hdr->size; ++k) {
        if (k == 0) {
            // 第一个 delta 就是 first_doc_id（绝对值，相对0）
            prev = deltas[k];
        } else {
            prev = prev + deltas[k];
        }
        out.push_back(prev);
    }

    return sizeof(BlockHeader) + main_bytes + (size_t)hdr->exc_count * sizeof(PatchEntry);
}

// ─────────────────────────────────────────────────────────────────────────────
// 全量解压
// ─────────────────────────────────────────────────────────────────────────────

std::vector<DocId> PForDelta::decompress(const uint8_t* data,
                                          size_t         data_len,
                                          uint32_t       total_docs)
{
    std::vector<DocId> result;
    result.reserve(total_docs);

    size_t offset = 0;
    while (offset < data_len && result.size() < total_docs) {
        size_t consumed = decompressBlock(data + offset, result);
        offset += consumed;
    }
    return result;
}

} // namespace ii
