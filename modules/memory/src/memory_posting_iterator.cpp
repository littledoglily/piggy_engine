#include "memory/memory_posting_iterator.h"
#include <algorithm>

namespace ii::memory {

MemoryPostingIterator::MemoryPostingIterator(std::vector<ii::DocId>  sorted_docs,
                                             std::vector<uint32_t>   sorted_tfs,
                                             float                   upper_bound)
    : docs_(std::move(sorted_docs))
    , tfs_(std::move(sorted_tfs))
    , upper_bound_(upper_bound)
{
    if (!docs_.empty()) {
        cur_doc_ = docs_[0];
        cur_tf_  = tfs_.empty() ? 1u : tfs_[0];
        pos_     = 1;
    }
}

bool MemoryPostingIterator::next() {
    if (pos_ >= docs_.size()) {
        cur_doc_ = ii::INVALID_DOC;
        return false;
    }
    cur_doc_ = docs_[pos_];
    cur_tf_  = (pos_ < tfs_.size()) ? tfs_[pos_] : 1u;
    ++pos_;
    return true;
}

bool MemoryPostingIterator::advance(ii::DocId target) {
    if (cur_doc_ == ii::INVALID_DOC) return false;
    if (cur_doc_ >= target)          return true;

    // Binary search in docs_[pos_-1 ..] (pos_ already points past cur_doc_)
    auto begin = docs_.begin() + pos_;
    auto it    = std::lower_bound(begin, docs_.end(), target);
    if (it == docs_.end()) {
        cur_doc_ = ii::INVALID_DOC;
        return false;
    }
    pos_     = static_cast<size_t>(it - docs_.begin());
    cur_doc_ = docs_[pos_];
    cur_tf_  = (pos_ < tfs_.size()) ? tfs_[pos_] : 1u;
    ++pos_;
    return true;
}

} // namespace ii::memory
