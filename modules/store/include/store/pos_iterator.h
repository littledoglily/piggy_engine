#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// store/pos_iterator.h  —  .pos_<field> 流式迭代器
//
// 每次 next() 只向前读一个文档的 (doc_id, tf, positions)，
// 不把整个 term 的 positions 加载进内存，供 K-way merge 使用。
//
// 对比 readPosEntries()：
//   readPosEntries  → 一次性加载全部，O(df × avgTF) 内存峰值
//   PosIterator     → 常数内存，流式消费，适合大 df 词
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include <fstream>
#include <string>
#include <vector>

namespace ii {

class PosIterator {
public:
    // 默认构造 = 已结束的空迭代器
    PosIterator() = default;

    // 打开 pos_path，从 pos_offset 处开始，最多读 doc_freq 条
    PosIterator(const std::string& pos_path,
                uint64_t           pos_offset,
                uint32_t           doc_freq);

    PosIterator(const PosIterator&)            = delete;
    PosIterator& operator=(const PosIterator&) = delete;
    PosIterator(PosIterator&&)                 = default;
    PosIterator& operator=(PosIterator&&)      = default;

    bool     isEnd()      const { return remaining_ == 0 && !preloaded_; }
    DocId    docId()      const { return cur_doc_id_; }
    uint32_t tf()         const { return cur_tf_; }

    // 当前文档的 positions（只在 next() / 构造后有效，next() 后失效）
    const std::vector<Pos>& positions() const { return cur_positions_; }

    // 移出当前 positions（避免拷贝），调用后 cur_positions_ 变空
    std::vector<Pos> takePositions() { return std::move(cur_positions_); }

    // 推进到下一个文档
    void next();

private:
    void loadNext();   // 从文件读下一条 entry 到 cur_*

    std::ifstream    file_;
    uint32_t         remaining_   = 0;     // 还剩几条未读
    bool             preloaded_   = false; // 当前 cur_* 是否有效
    DocId            cur_doc_id_  = 0;
    uint32_t         cur_tf_      = 0;
    std::vector<Pos> cur_positions_;
};

} // namespace ii
