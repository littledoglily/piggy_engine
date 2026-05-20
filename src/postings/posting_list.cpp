#include "store/posting_list.h"
#include <algorithm>
#include <stdexcept>

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// PostingList
// ─────────────────────────────────────────────────────────────────────────────

void PostingList::append(DocId doc_id, Pos position) {
    auto it = doc_index_.find(doc_id);
    if (it == doc_index_.end()) {
        // 新文档：追加一个 PostingEntry
        PostingEntry e;
        e.doc_id = doc_id;
        e.tf     = 1;
        e.positions.push_back(position);
        doc_index_[doc_id] = entries_.size();
        entries_.push_back(std::move(e));
    } else {
        // 同一文档再次出现该 term：累加 tf，追加 position
        PostingEntry& e = entries_[it->second];
        e.tf++;
        e.positions.push_back(position);
    }
}

std::vector<DocId> PostingList::docIds() const {
    // entries_ 已按写入顺序（写入时 doc_id 单调递增）排列
    // 如果多线程写或乱序写需要排序，这里保险起见排一次
    std::vector<DocId> ids;
    ids.reserve(entries_.size());
    for (const auto& e : entries_) ids.push_back(e.doc_id);
    // 确保升序（单线程顺序写入时本来就是有序的）
    std::sort(ids.begin(), ids.end());
    return ids;
}

uint32_t PostingList::totalTermFreq() const {
    uint32_t total = 0;
    for (const auto& e : entries_) total += e.tf;
    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
// InMemoryIndex
// ─────────────────────────────────────────────────────────────────────────────

void InMemoryIndex::addToken(const Token& tok) {
    index_[tok.term].append(tok.doc_id, tok.position);
}

std::vector<std::string> InMemoryIndex::sortedTerms() const {
    std::vector<std::string> terms;
    terms.reserve(index_.size());
    for (const auto& kv : index_) terms.push_back(kv.first);
    std::sort(terms.begin(), terms.end());
    return terms;
}

const PostingList* InMemoryIndex::getPostingList(const std::string& term) const {
    auto it = index_.find(term);
    if (it == index_.end()) return nullptr;
    return &it->second;
}

} // namespace ii
