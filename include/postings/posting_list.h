#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// posting_list.h  —  内存倒排链（flush 前的可变结构）
//
// 写入路径：
//   IndexWriter::addDocument()
//     → Analyzer 产出 Token 流
//     → PostingList::append(doc_id, tf, positions)
//
// 读取路径（flush 时）：
//   PostingList::docIds()   → 升序 doc_id 列表（送入 PForDelta 压缩）
//   PostingList::entries()  → 完整 PostingEntry 列表（含 tf/positions）
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include <unordered_map>

namespace ii {

// ── 内存中单个 term 的倒排链 ──────────────────────────────────────────────────
class PostingList {
public:
    // 追加一个 <doc_id, position> 记录
    // 同一 doc 多次调用会累加 tf 并追加 position
    void append(DocId doc_id, Pos position);

    // 返回所有 doc_id（升序，用于 PForDelta 压缩）
    std::vector<DocId> docIds() const;

    // 返回完整 PostingEntry 列表
    const std::vector<PostingEntry>& entries() const { return entries_; }

    size_t   size()  const { return entries_.size(); }
    bool     empty() const { return entries_.empty(); }
    uint32_t totalTermFreq() const;

private:
    std::vector<PostingEntry>        entries_;     // 按 doc_id 升序
    std::unordered_map<DocId, size_t> doc_index_;  // doc_id → entries_ 下标
};

// ── 内存倒排索引：term → PostingList ─────────────────────────────────────────
class InMemoryIndex {
public:
    // 写入一个 Token
    void addToken(const Token& tok);

    // 获取所有 term（排序后，flush 时按字母序写 .tim）
    std::vector<std::string> sortedTerms() const;

    // 获取指定 term 的倒排链
    const PostingList* getPostingList(const std::string& term) const;

    size_t termCount() const { return index_.size(); }
    size_t docCount()  const { return doc_count_; }

    // 更新文档计数（IndexWriter 调用）
    void setDocCount(size_t n) { doc_count_ = n; }

private:
    std::unordered_map<std::string, PostingList> index_;
    size_t doc_count_ = 0;
};

} // namespace ii
