#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// store/i_posting_iterator.h  —  PostingList 迭代器抽象接口
//
// 所有 posting 迭代器的公共合约：
//   - PostingIterator  (disk)   — 文件版，解压 PForDelta 块
//   - MemoryPostingIterator     — 内存版，遍历 TermPage 链
//
// ISegmentReader::postingIterator() 返回 unique_ptr<IPostingIterator>，
// 上层 query/ 模块只依赖本接口，不感知底层实现。
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include <memory>

namespace ii {

class IPostingIterator {
public:
    virtual ~IPostingIterator() = default;

    virtual DocId    docId()             const = 0;
    virtual uint32_t tf()                const = 0;

    // Block-level upper bound for BlockMaxWAND (without IDF factor).
    virtual float    blockMaxScore()     const = 0;
    // Largest docId in the current block (INVALID_DOC if unknown/exhausted).
    virtual DocId    blockMaxDocId()     const = 0;

    virtual bool     isEnd()             const = 0;

    // Advance to next doc. Returns false (and sets isEnd()) when exhausted.
    virtual bool next()                    = 0;
    // Advance to first doc >= target. Returns false when no such doc exists.
    virtual bool advance(DocId target)     = 0;

    // Factory: returns an always-exhausted iterator (isEnd() == true).
    // Use instead of a default-constructed concrete iterator.
    static std::unique_ptr<IPostingIterator> makeEmpty();
};

} // namespace ii
