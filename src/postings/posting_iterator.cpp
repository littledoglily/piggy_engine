#include "postings/posting_iterator.h"
#include "postings/pfor_delta.h"
#include <cstring>
#include <stdexcept>

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// 构造：打开文件 → 读 SkipList → seek 到 posting 起点 → 推进到第一个 doc
// ─────────────────────────────────────────────────────────────────────────────

PostingIterator::PostingIterator(const TermMeta& meta, const std::string& doc_path)
    : meta_(meta)
{
    file_.open(doc_path, std::ios::binary);
    if (!file_) throw std::runtime_error("PostingIterator: cannot open " + doc_path);

    // ── 读取 SkipList（格式与 SegmentReader::readSkipList 完全一致）──────────
    file_.seekg(static_cast<std::streamoff>(meta_.skip_offset));
    uint32_t l0c = 0, l1c = 0;
    file_.read(reinterpret_cast<char*>(&l0c), 4);
    file_.read(reinterpret_cast<char*>(&l1c), 4);
    size_t skip_bytes = 8 + (size_t)(l0c + l1c) * sizeof(SkipNode);
    std::vector<uint8_t> buf(skip_bytes);
    file_.seekg(static_cast<std::streamoff>(meta_.skip_offset));
    file_.read(reinterpret_cast<char*>(buf.data()), skip_bytes);
    skip_list_ = SkipList::deserialize(buf.data(), buf.size());

    // ── Seek 到 Posting 起点，初始化状态 ─────────────────────────────────────
    remaining_ = meta_.doc_freq;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(meta_.posting_offset));

    next();  // 推进到第一个 doc，构造后 docId() 即可用
}

// ─────────────────────────────────────────────────────────────────────────────
// loadNextBlock：从当前文件位置解压下一个 Block
// ─────────────────────────────────────────────────────────────────────────────

bool PostingIterator::loadNextBlock() {
    if (remaining_ == 0) { cur_doc_ = INVALID_DOC; return false; }

    BlockHeader hdr;
    file_.read(reinterpret_cast<char*>(&hdr), sizeof(BlockHeader));
    if (!file_) { cur_doc_ = INVALID_DOC; return false; }

    size_t main_bytes  = ((size_t)hdr.size * hdr.b + 7) / 8;
    size_t patch_bytes = (size_t)hdr.exc_count * sizeof(PatchEntry);

    std::vector<uint8_t> block_data(sizeof(BlockHeader) + main_bytes + patch_bytes);
    std::memcpy(block_data.data(), &hdr, sizeof(BlockHeader));
    file_.read(reinterpret_cast<char*>(block_data.data() + sizeof(BlockHeader)),
               main_bytes + patch_bytes);

    cur_block_.clear();
    PForDelta::decompressBlock(block_data.data(), cur_block_);

    remaining_ -= std::min(remaining_, (uint32_t)hdr.size);

    // max_score 来自 SkipList Level0 节点（= Block 内 max_tf_norm）
    block_max_score_ = (cur_block_idx_ < skip_list_.size())
                       ? skip_list_.node(cur_block_idx_).max_score
                       : 0.0f;
    ++cur_block_idx_;
    cur_pos_ = 0;
    return !cur_block_.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// next：推进到下一个 doc
// ─────────────────────────────────────────────────────────────────────────────

bool PostingIterator::next() {
    // 当前 Block 还有剩余
    if (cur_pos_ < cur_block_.size()) {
        cur_doc_ = cur_block_[cur_pos_++];
        return true;
    }
    // 加载下一个 Block
    if (!loadNextBlock()) return false;
    cur_doc_ = cur_block_[cur_pos_++];
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// advance：跳跃到第一个 >= target 的 doc
// ─────────────────────────────────────────────────────────────────────────────

bool PostingIterator::advance(DocId target) {
    if (cur_doc_ == INVALID_DOC) return false;
    if (cur_doc_ >= target)      return true;

    // 先在当前 Block 内线性扫描
    while (cur_pos_ < cur_block_.size()) {
        if (cur_block_[cur_pos_] >= target) {
            cur_doc_ = cur_block_[cur_pos_++];
            return true;
        }
        ++cur_pos_;
    }

    // 当前 Block 内找不到，用 SkipList 跳跃到正确的 Block
    return seekToBlock(target);
}

// ─────────────────────────────────────────────────────────────────────────────
// seekToBlock：SkipList 跳跃 + 块内扫描
// ─────────────────────────────────────────────────────────────────────────────

bool PostingIterator::seekToBlock(DocId target) {
    // 没有 SkipList（整个 posting 只有 1 个 Block），顺序加载剩余 Block
    if (skip_list_.empty()) {
        while (loadNextBlock()) {
            for (; cur_pos_ < cur_block_.size(); ++cur_pos_) {
                if (cur_block_[cur_pos_] >= target) {
                    cur_doc_ = cur_block_[cur_pos_++];
                    return true;
                }
            }
        }
        cur_doc_ = INVALID_DOC;
        return false;
    }

    auto result = skip_list_.find(target);

    if (result.byte_offset == UINT64_MAX) {
        // target 超出 SkipList 覆盖范围，顺序加载剩余 Block
        while (loadNextBlock()) {
            for (; cur_pos_ < cur_block_.size(); ++cur_pos_) {
                if (cur_block_[cur_pos_] >= target) {
                    cur_doc_ = cur_block_[cur_pos_++];
                    return true;
                }
            }
        }
        cur_doc_ = INVALID_DOC;
        return false;
    }

    // 目标 Block 比当前加载位置更靠前（不应发生，防御性处理）
    if (result.block_index < cur_block_idx_) {
        // 只能在当前已加载内容内顺序查找
        while (loadNextBlock()) {
            for (; cur_pos_ < cur_block_.size(); ++cur_pos_) {
                if (cur_block_[cur_pos_] >= target) {
                    cur_doc_ = cur_block_[cur_pos_++];
                    return true;
                }
            }
        }
        cur_doc_ = INVALID_DOC;
        return false;
    }

    // ── 向目标 Block 跳跃 ─────────────────────────────────────────────────────
    // 计算目标 Block 之前共有多少 doc（用于重置 remaining_）
    uint32_t docs_before = 0;
    for (size_t i = 0; i < result.block_index; ++i)
        docs_before += (i < skip_list_.size()) ? skip_list_.node(i).doc_count : 128u;

    remaining_     = (meta_.doc_freq > docs_before) ? meta_.doc_freq - docs_before : 0;
    cur_block_idx_ = result.block_index;
    cur_block_.clear();
    cur_pos_ = 0;

    file_.clear();
    file_.seekg(static_cast<std::streamoff>(meta_.posting_offset + result.byte_offset));

    // 加载目标 Block 并在块内扫描
    while (loadNextBlock()) {
        for (; cur_pos_ < cur_block_.size(); ++cur_pos_) {
            if (cur_block_[cur_pos_] >= target) {
                cur_doc_ = cur_block_[cur_pos_++];
                return true;
            }
        }
        // target 不在本 Block（SkipList 精度内不应发生），继续下一 Block
    }

    cur_doc_ = INVALID_DOC;
    return false;
}

} // namespace ii
