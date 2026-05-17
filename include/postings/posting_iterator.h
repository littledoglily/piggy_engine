#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// posting_iterator.h  —  Posting List 惰性迭代器
//
// 职责：
//   替代 readPostingList() 的全量解压路径，每次只保留一个 Block（128 doc）在内存。
//   通过 SkipList 支持 advance(target) 跳跃，跳过不相关的 Block。
//
// 生命期约束：
//   每个 PostingIterator 持有独立的 std::ifstream（通过 doc_path 打开），
//   与 SegmentReader 的共享 doc_file_ 完全独立，多个迭代器可并发使用。
//
// 接口语义：
//   构造后 docId() 指向第一个 doc（已调用一次 next()）。
//   next()      : 推进到下一个 doc，返回 false 表示耗尽。
//   advance(t)  : 跳到第一个 >= t 的 doc，返回 false 表示不存在。
//   blockMaxScore() : 当前 Block 的 SkipNode.max_score（= max_tf_norm）。
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include "postings/skiplist.h"
#include <fstream>
#include <string>
#include <vector>

namespace ii {

class PostingIterator {
public:
    // 打开 doc_path，加载 SkipList，推进到第一个 doc
    PostingIterator(const TermMeta& meta, const std::string& doc_path);

    // 空迭代器（isEnd() == true）
    PostingIterator() = default;

    // 不可拷贝（持有独立文件句柄），可移动
    PostingIterator(const PostingIterator&)            = delete;
    PostingIterator& operator=(const PostingIterator&) = delete;
    PostingIterator(PostingIterator&&)                 = default;
    PostingIterator& operator=(PostingIterator&&)      = default;

    DocId docId()         const { return cur_doc_; }
    float blockMaxScore() const { return block_max_score_; }
    bool  isEnd()         const { return cur_doc_ == INVALID_DOC; }

    // 推进到下一个 doc；return false 表示耗尽
    bool next();

    // 跳跃到第一个 >= target 的 doc；return false 表示不存在
    bool advance(DocId target);

private:
    // 从当前文件位置解压下一个 Block 到 cur_block_
    // 同步更新 remaining_、cur_block_idx_、block_max_score_
    bool loadNextBlock();

    // 用 SkipList 定位到包含 target 的 Block 并加载，
    // 然后在块内扫描到第一个 >= target 的 doc
    bool seekToBlock(DocId target);

    TermMeta           meta_;                    // 值拷贝，生命期独立于 SegmentReader
    std::ifstream      file_;                    // 独立文件句柄

    SkipList           skip_list_;
    uint32_t           remaining_       = 0;     // 剩余未解压的 doc 数
    size_t             cur_block_idx_   = 0;     // 下一个要加载的 Block 序号（0-indexed）
    std::vector<DocId> cur_block_;               // 当前已解压 Block 的 doc_id 列表
    size_t             cur_pos_         = 0;     // cur_block_ 中下一个待返回的下标
    DocId              cur_doc_         = INVALID_DOC;
    float              block_max_score_ = 0.0f;
};

} // namespace ii
