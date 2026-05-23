#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// memory/memory_posting_iterator.h  —  内存版 PostingList 迭代器
//
// 从 TermPage 链收集所有 (doc_id, tf) 并按 doc_id 升序存储后，
// 直接在 sorted vector 上实现 next() / advance()，无磁盘 IO。
//
// blockMaxScore() 返回 upper_bound（整个列表视为一个 block），
// blockMaxDocId() 返回列表最后一个 doc_id，供 BlockMaxWAND 使用。
// ─────────────────────────────────────────────────────────────────────────────
#include "store/i_posting_iterator.h"
#include "core/types.h"
#include <vector>

namespace ii::memory {

class MemoryPostingIterator : public ii::IPostingIterator {
public:
    // sorted_docs must be ascending; sorted_tfs[i] is the tf for sorted_docs[i].
    // upper_bound is the pre-computed BM25 upper bound for BlockMaxWAND.
    MemoryPostingIterator(std::vector<ii::DocId>  sorted_docs,
                          std::vector<uint32_t>   sorted_tfs,
                          float                   upper_bound);

    ii::DocId docId()         const override { return cur_doc_; }
    uint32_t  tf()            const override { return cur_tf_;  }
    float     blockMaxScore() const override { return upper_bound_; }
    ii::DocId blockMaxDocId() const override {
        return docs_.empty() ? ii::INVALID_DOC : docs_.back();
    }
    bool      isEnd()         const override { return cur_doc_ == ii::INVALID_DOC; }

    bool next()                override;
    bool advance(ii::DocId target) override;

private:
    std::vector<ii::DocId>  docs_;
    std::vector<uint32_t>   tfs_;
    size_t                  pos_        = 0;
    ii::DocId               cur_doc_    = ii::INVALID_DOC;
    uint32_t                cur_tf_     = 1;
    float                   upper_bound_ = 0.0f;
};

} // namespace ii::memory
