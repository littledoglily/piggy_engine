#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// store/posting_list.h  —  内存倒排链（flush 前的可变结构）
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include <unordered_map>

namespace ii {

class PostingList {
public:
    size_t append(DocId doc_id, Pos position);

    std::vector<DocId> docIds() const;
    const std::vector<PostingEntry>& entries() const { return entries_; }

    size_t   size()          const { return entries_.size(); }
    bool     empty()         const { return entries_.empty(); }
    uint32_t totalTermFreq() const;

private:
    std::vector<PostingEntry>         entries_;
    std::unordered_map<DocId, size_t> doc_index_;
};

class InMemoryIndex {
public:
    void addToken(const Token& tok);

    std::vector<std::string> sortedTerms() const;
    const PostingList* getPostingList(const std::string& term) const;

    size_t termCount() const { return index_.size(); }
    size_t docCount()  const { return doc_count_; }
    void   setDocCount(size_t n) { doc_count_ = n; }
    size_t ramUsage()  const { return ram_bytes_; }

private:
    std::unordered_map<std::string, PostingList> index_;
    size_t doc_count_ = 0;
    size_t ram_bytes_ = 0;
};

} // namespace ii
