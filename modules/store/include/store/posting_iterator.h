#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// store/posting_iterator.h  —  Posting List 惰性迭代器
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "codec/skiplist.h"
#include <fstream>
#include <string>
#include <vector>

namespace ii {

class PostingIterator {
public:
    PostingIterator(const TermMeta& meta, const std::string& doc_path);
    PostingIterator() = default;

    PostingIterator(const PostingIterator&)            = delete;
    PostingIterator& operator=(const PostingIterator&) = delete;
    PostingIterator(PostingIterator&&)                 = default;
    PostingIterator& operator=(PostingIterator&&)      = default;

    DocId    docId()         const { return cur_doc_; }
    float    blockMaxScore() const { return block_max_score_; }
    DocId    blockMaxDocId() const { return cur_block_.empty() ? INVALID_DOC : cur_block_.back(); }
    bool     isEnd()         const { return cur_doc_ == INVALID_DOC; }
    uint32_t tf()            const { return cur_tf_; }

    bool next();
    bool advance(DocId target);

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
