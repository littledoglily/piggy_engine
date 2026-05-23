#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// store/posting_iterator.h  —  磁盘版 Posting List 惰性迭代器
//
// 实现 IPostingIterator；内部通过 PForDelta 块解压 + SkipList 跳跃。
// 创建：SegmentReader::postingIterator() 返回 unique_ptr<IPostingIterator>。
// 空迭代器：使用 IPostingIterator::makeEmpty()，不要直接默认构造本类。
// ─────────────────────────────────────────────────────────────────────────────
#include "store/i_posting_iterator.h"
#include "core/types.h"
#include "codec/skiplist.h"
#include <fstream>
#include <string>
#include <vector>

namespace ii {

class PostingIterator : public IPostingIterator {
public:
    PostingIterator(const TermMeta& meta, const std::string& doc_path);

    PostingIterator(const PostingIterator&)            = delete;
    PostingIterator& operator=(const PostingIterator&) = delete;
    PostingIterator(PostingIterator&&)                 = default;
    PostingIterator& operator=(PostingIterator&&)      = default;

    DocId    docId()         const override { return cur_doc_; }
    float    blockMaxScore() const override { return block_max_score_; }
    DocId    blockMaxDocId() const override {
        return cur_block_.empty() ? INVALID_DOC : cur_block_.back();
    }
    bool     isEnd()         const override { return cur_doc_ == INVALID_DOC; }
    uint32_t tf()            const override { return cur_tf_; }

    bool next()                override;
    bool advance(DocId target) override;

private:
    bool loadNextBlock();
    bool seekToBlock(DocId target);
    bool scanBlock(DocId target);

    TermMeta           meta_;
    std::ifstream      file_;
    SkipList           skip_list_;
    uint32_t           remaining_       = 0;
    size_t             cur_block_idx_   = 0;
    std::vector<DocId> cur_block_;
    size_t             cur_pos_         = 0;
    DocId              cur_doc_         = INVALID_DOC;
    float              block_max_score_ = 0.0f;

    std::vector<uint8_t> all_tfs_;
    std::vector<uint8_t> cur_tf_block_;
    uint32_t             cur_tf_ = 1;
};

} // namespace ii
