# 惰性 Posting 迭代器设计

## 现状与问题

`SegmentReader::readPostingList()` 将某个 term 的所有 Block 一次性解压到 `vector<DocId>`，内存复杂度 O(df)。对于 df=800K 的高频词，单次查询要分配数 MB 内存并全部解压，即使 WAND 最终只访问其中极少一部分。

```
// segment_reader.cpp — 当前全量解压路径
std::vector<DocId> result;
result.reserve(meta->doc_freq);   // ← O(df) 预分配
while (remaining > 0) {
    PForDelta::decompressBlock(..., result);  // 逐 Block append
    remaining -= hdr.size;
}
return result;  // 全部 doc_id 在内存中
```

`searchAND` 和 `searchOR_WAND` 都走这条路径，SkipList 只在 `readPostingListFrom` 中有限使用。

---

## 设计目标

引入 `PostingIterator`，按需解压，同一时刻只保留当前 Block（128 个 doc_id），内存 O(1)。提供：

- `next()` — 推进到下一个 doc，跨 Block 时自动解压下一 Block
- `advance(target)` — 利用 SkipList 跳过若干 Block，直达 `>= target` 的 doc
- `blockMaxScore()` — 返回当前 Block 的 `SkipNode.max_score`（供 BlockMaxWAND 读取）

---

## 核心数据结构

```cpp
// include/postings/posting_iterator.h

class PostingIterator {
public:
    // 从 SegmentReader 的已打开 .doc 文件构造
    // meta      : term 元数据（posting_offset、skip_offset、doc_freq）
    // doc_file  : 已打开的 .doc ifstream 引用（由 SegmentReader 持有）
    PostingIterator(const TermMeta& meta, std::ifstream& doc_file);

    // 当前 doc_id，未初始化或已结束时返回 INVALID_DOC
    DocId docId() const;

    // 推进到下一个 doc；返回 false 表示已耗尽
    bool next();

    // 跳跃到第一个 >= target 的 doc（利用 SkipList 定位 Block）
    // 返回 false 表示不存在 >= target 的 doc
    bool advance(DocId target);

    // 当前 Block 的 max_score（SkipNode.max_score，即 max_tf_norm）
    // 供 BlockMaxWAND 使用；在 next()/advance() 跨 Block 后自动更新
    float blockMaxScore() const;

    bool isEnd() const { return cur_doc_ == INVALID_DOC; }

private:
    // 解压下一个 Block 到 cur_block_，更新 cur_block_idx_、block_max_score_
    bool loadNextBlock();

    // 用 SkipList 定位到包含 target 的 Block，seek doc_file_ 并解压
    bool seekToBlock(DocId target);

    const TermMeta&   meta_;
    std::ifstream&    doc_file_;
    SkipList          skip_list_;     // 从磁盘懒加载（构造时读取）

    uint32_t          remaining_;     // 剩余未解压的 doc 数
    size_t            cur_block_idx_; // 当前 Block 在 Level0 中的序号
    std::vector<DocId> cur_block_;    // 当前已解压的 Block（最多 128 个元素）
    size_t            cur_pos_;       // 在 cur_block_ 中的位置
    DocId             cur_doc_;       // 当前 docId()
    float             block_max_score_; // 当前 Block 的 max_score
};
```

---

## 状态机

```
构造时：remaining_ = meta.doc_freq，cur_doc_ = INVALID_DOC
         skip_list_ 从 doc_file_ seek(meta.skip_offset) 读取
         doc_file_ seek 到 meta.posting_offset 等待第一次 next()

next() 调用：
  if cur_pos_ < cur_block_.size():
      cur_doc_ = cur_block_[cur_pos_++]
  else:
      loadNextBlock()  // 解压下一 Block，remaining_ -= block.size
      cur_doc_ = cur_block_[0], cur_pos_ = 1

advance(target)：
  if cur_doc_ >= target: return true   // 无需移动
  if target > skip_list_.lastMaxDoc(): return false  // 越界
  
  result = skip_list_.find(target)
  if result.block_index > cur_block_idx_:
      // 需要跳跃
      skip forward remaining blocks in accounting
      doc_file_.seekg(meta.posting_offset + result.byte_offset)
      cur_block_idx_ = result.block_index
      loadNextBlock()
  
  // 在当前 Block 内线性推进到 >= target
  while cur_doc_ < target && cur_pos_ < cur_block_.size():
      cur_doc_ = cur_block_[cur_pos_++]
  
  return cur_doc_ >= target
```

---

## SegmentReader 接口变更

新增工厂方法，保留旧接口向后兼容：

```cpp
// 新接口（惰性）
PostingIterator postingIterator(const std::string& term) const;

// 旧接口保留（全量，供测试/统计工具继续用）
std::vector<DocId> readPostingList(const std::string& term) const;
```

`IndexSearcher::searchAND` 和 `searchOR_WAND` 改用 `postingIterator()`；`wiki_indexer` 的统计打印继续用 `readPostingList()`。

---

## TermCursor 变更（影响 WAND）

```cpp
// 原
struct TermCursor {
    std::vector<DocId>  docs;
    size_t              ptr;
    ...
};

// 改
struct TermCursor {
    PostingIterator  iter;    // 惰性迭代器，替换 docs+ptr
    float            ub;     // 动态更新（BlockMaxWAND 会修改它）
    ...
    DocId curDoc() const { return iter.docId(); }
};
```

---

## 验证方案

### 正确性验证

新建 `tests/postings/test_posting_iterator.cpp`：

1. **与全量结果一致**：对同一 term，`PostingIterator` 逐个 `next()` 的结果与 `readPostingList()` 完全一致
2. **advance 正确性**：对所有偶数 doc_id 做 `advance(2k+1)`，验证结果和 `readPostingList()` 里 `lower_bound(2k+1)` 一致
3. **跨 Block 边界**：构造 df=256 的 term（覆盖 2 个 Block），在 Block 边界前后各做一次 `advance`，验证正确跳转
4. **advance 越界**：`advance(UINT32_MAX)` 返回 false，`isEnd()` 为 true

```cpp
// 测试模板
auto iter = seg_reader.postingIterator("some_term");
auto full  = seg_reader.readPostingList("some_term");

size_t i = 0;
while (iter.next()) {
    ASSERT(iter.docId() == full[i++]);
}
ASSERT(i == full.size());
```

### 性能对比

索引 10K+ 篇文档后，对 df > 10000 的高频词分别用旧接口和新接口查询，记录：
- 峰值内存（RSS）
- 单次查询耗时

期望：内存降低 ~90%，耗时与 df 无关（取决于实际访问的 Block 数）。

---

## 不在本文档范围内

- **BlockMaxWAND**：`blockMaxScore()` 接口已预留，但消费逻辑见 `blockmax_wand.md`
- **位置信息惰性加载**：`.pos` 文件的惰性读取可独立设计
- **多线程安全**：`PostingIterator` 单线程使用，`SegmentReader` 的 `doc_file_` mutable ifstream 需外部保证不并发
