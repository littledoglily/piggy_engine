#include "store/posting_list.h"
#include <algorithm>
#include <stdexcept>

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// PostingList
// ─────────────────────────────────────────────────────────────────────────────

size_t PostingList::append(DocId doc_id, Pos position) {
    size_t mem_use = 0;
    auto it = doc_index_.find(doc_id);
    if (it == doc_index_.end()) {
        // 新文档：追加一个 PostingEntry
        PostingEntry e;
        e.doc_id = doc_id;
        e.tf     = 1;
        e.positions.push_back(position);
        doc_index_[doc_id] = entries_.size();
        entries_.push_back(std::move(e));
        // 内存使用新增：PostingEntry 结构体 + doc_id 索引（不考虑 positions 向量的动态分配开销，通常较小且不频繁）
        mem_use += sizeof(PostingEntry) + sizeof(doc_id);
    } else {
        // 同一文档再次出现该 term：累加 tf，追加 position
        PostingEntry& e = entries_[it->second];
        e.tf++;
        e.positions.push_back(position);
        mem_use += sizeof(Pos);  // 仅考虑 positions 向量新增的 position 大小
    }
    return mem_use;
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
    auto it = index_.find(tok.term);
    if (it == index_.end()) {
        ram_bytes_ += tok.term.size();  // string key in index_
        ram_bytes_ += index_[tok.term].append(tok.doc_id, tok.position);
    } else {
        ram_bytes_ += it->second.append(tok.doc_id, tok.position);
    }
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
