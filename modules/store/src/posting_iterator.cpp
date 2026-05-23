#include "store/posting_iterator.h"
#include "codec/pfor_delta.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {

// Always-exhausted iterator — returned by IPostingIterator::makeEmpty().
class EmptyPostingIterator : public ii::IPostingIterator {
public:
    ii::DocId docId()         const override { return ii::INVALID_DOC; }
    uint32_t  tf()            const override { return 1; }
    float     blockMaxScore() const override { return 0.0f; }
    ii::DocId blockMaxDocId() const override { return ii::INVALID_DOC; }
    bool      isEnd()         const override { return true; }
    bool      next()                override { return false; }
    bool      advance(ii::DocId)    override { return false; }
};

} // anonymous namespace

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// IPostingIterator::makeEmpty
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<IPostingIterator> IPostingIterator::makeEmpty() {
    return std::make_unique<EmptyPostingIterator>();
}

// ─────────────────────────────────────────────────────────────────────────────
// PostingIterator 构造：打开文件 → 读 SkipList → seek 到 posting 起点 → 推进到第一个 doc
// ─────────────────────────────────────────────────────────────────────────────

PostingIterator::PostingIterator(const TermMeta& meta, const std::string& doc_path)
    : meta_(meta)
{
    file_.open(doc_path, std::ios::binary);
    if (!file_) throw std::runtime_error("PostingIterator: cannot open " + doc_path);

    // ── 读取 SkipList ─────────────────────────────────────────────────────────
    file_.seekg(static_cast<std::streamoff>(meta_.skip_offset));
    uint32_t l0c = 0, l1c = 0;
    file_.read(reinterpret_cast<char*>(&l0c), 4);
    file_.read(reinterpret_cast<char*>(&l1c), 4);
    size_t skip_bytes = 8 + (size_t)(l0c + l1c) * sizeof(SkipNode);
    std::vector<uint8_t> buf(skip_bytes);
    file_.seekg(static_cast<std::streamoff>(meta_.skip_offset));
    file_.read(reinterpret_cast<char*>(buf.data()), skip_bytes);
    skip_list_ = SkipList::deserialize(buf.data(), buf.size());

    // ── 预加载 tf 字节数组 ────────────────────────────────────────────────────
    if (meta_.tf_data_offset > 0 && meta_.doc_freq > 0) {
        all_tfs_.resize(meta_.doc_freq);
        file_.seekg(static_cast<std::streamoff>(meta_.tf_data_offset));
        file_.read(reinterpret_cast<char*>(all_tfs_.data()), meta_.doc_freq);
    }

    // ── Seek 到 Posting 起点，推进到第一个 doc ────────────────────────────────
    remaining_ = meta_.doc_freq;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(meta_.posting_offset));

    next();
}

// ─────────────────────────────────────────────────────────────────────────────
// scanBlock：在 cur_block_[cur_pos_..] 内二分查找第一个 >= target 的 doc
// ─────────────────────────────────────────────────────────────────────────────

bool PostingIterator::scanBlock(DocId target) {
    auto begin = cur_block_.begin() + cur_pos_;
    auto it    = std::lower_bound(begin, cur_block_.end(), target);
    if (it == cur_block_.end()) {
        cur_pos_ = cur_block_.size();
        return false;
    }
    size_t idx = static_cast<size_t>(it - cur_block_.begin());
    cur_doc_ = *it;
    cur_tf_  = (!cur_tf_block_.empty() && idx < cur_tf_block_.size())
               ? cur_tf_block_[idx] : 1u;
    cur_pos_ = idx + 1;
    return true;
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

    block_max_score_ = (cur_block_idx_ < skip_list_.size())
                       ? skip_list_.node(cur_block_idx_).max_score
                       : 0.0f;

    if (!all_tfs_.empty()) {
        size_t block_start = cur_block_idx_ * (size_t)PForDelta::BLOCK_SIZE;
        size_t block_size  = cur_block_.size();
        if (block_start < all_tfs_.size()) {
            size_t avail = std::min(block_size, all_tfs_.size() - block_start);
            cur_tf_block_.assign(all_tfs_.begin() + block_start,
                                 all_tfs_.begin() + block_start + avail);
        } else {
            cur_tf_block_.clear();
        }
    } else {
        cur_tf_block_.clear();
    }

    ++cur_block_idx_;
    cur_pos_ = 0;
    return !cur_block_.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// next
// ─────────────────────────────────────────────────────────────────────────────

bool PostingIterator::next() {
    if (cur_pos_ < cur_block_.size()) {
        cur_tf_  = (!cur_tf_block_.empty() && cur_pos_ < cur_tf_block_.size())
                   ? cur_tf_block_[cur_pos_] : 1u;
        cur_doc_ = cur_block_[cur_pos_++];
        return true;
    }
    if (!loadNextBlock()) return false;
    cur_tf_  = (!cur_tf_block_.empty() && cur_pos_ < cur_tf_block_.size())
               ? cur_tf_block_[cur_pos_] : 1u;
    cur_doc_ = cur_block_[cur_pos_++];
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// advance
// ─────────────────────────────────────────────────────────────────────────────

bool PostingIterator::advance(DocId target) {
    if (cur_doc_ == INVALID_DOC) return false;
    if (cur_doc_ >= target)      return true;

    if (scanBlock(target)) return true;
    return seekToBlock(target);
}

// ─────────────────────────────────────────────────────────────────────────────
// seekToBlock
// ─────────────────────────────────────────────────────────────────────────────

bool PostingIterator::seekToBlock(DocId target) {
    if (skip_list_.empty()) {
        while (loadNextBlock()) {
            if (scanBlock(target)) return true;
        }
        cur_doc_ = INVALID_DOC;
        return false;
    }

    auto result = skip_list_.find(target);

    if (result.byte_offset == UINT64_MAX) {
        while (loadNextBlock()) {
            if (scanBlock(target)) return true;
        }
        cur_doc_ = INVALID_DOC;
        return false;
    }

    if (result.block_index < cur_block_idx_) {
        while (loadNextBlock()) {
            if (scanBlock(target)) return true;
        }
        cur_doc_ = INVALID_DOC;
        return false;
    }

    uint32_t docs_before = 0;
    for (size_t i = 0; i < result.block_index; ++i)
        docs_before += (i < skip_list_.size()) ? skip_list_.node(i).doc_count : 128u;

    remaining_     = (meta_.doc_freq > docs_before) ? meta_.doc_freq - docs_before : 0;
    cur_block_idx_ = result.block_index;
    cur_block_.clear();
    cur_pos_ = 0;

    file_.clear();
    file_.seekg(static_cast<std::streamoff>(meta_.posting_offset + result.byte_offset));

    while (loadNextBlock()) {
        if (scanBlock(target)) return true;
    }

    cur_doc_ = INVALID_DOC;
    return false;
}

} // namespace ii
