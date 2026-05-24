# memory 模块

## 职责
实时内存索引：接收 `addDocument` 写入，在内存中维护倒排链、位置链、原文页链，并在触发条件时 flush 到磁盘 Segment。
实现 SWMR（单写多读）epoch 模型，保证写入与并发搜索的安全隔离。

## 对外接口

### MemorySegment（写侧）
```cpp
class MemorySegment {
    void     addDocument(const Document&);
    void     flushToDisk(const std::string& dir, uint32_t seg_id, const Schema&);
    void     reset();                    // 清空，准备下一个 epoch
    uint64_t ramUsed() const;
    uint32_t docCount() const;
    // 内部查询（供 MemorySegmentReader 访问）
    uint32_t docFieldLen(const std::string& field, DocId) const;
    uint64_t fieldTotalTokens(const std::string& field)  const;
    StoredPageHeader* storedHead(DocId)                  const;
    const std::string& docExtId(DocId)                   const;
};
```

### MemorySegmentReader（读侧，实现 ISegmentReader）
```cpp
// 构造方式一：引用绑定（caller 负责保证 MemorySegment 存活）
MemorySegmentReader(const MemorySegment&, uint32_t seg_id, const Schema&);

// 构造方式二：共享所有权（SWMR epoch 切换时使用，reader 共同持有 MemorySegment）
MemorySegmentReader(std::shared_ptr<MemorySegment>, uint32_t seg_id, const Schema&);

// ISegmentReader 接口
PostingIterator postingIterator(field, term) const override;
const TermMeta* getTermMeta(field, term)    const override;
uint32_t        fieldDocLen(field, doc_id)  const override;
float           fieldAvgDocLen(field)       const override;
uint32_t        docCount()                  const override;
uint32_t        segmentId()                 const override;
const vector<string>& indexedFieldNames()   const override;
bool            isAlive(DocId)              const override;
```

### MemoryPostingIterator
`blockMaxScore()` 返回整条链的最大分（整个链视为一个 block），与磁盘 SkipList 接口对齐，支持 WAND 上界剪枝。

## 内存数据结构

| 结构 | 用途 |
|------|------|
| `SegmentArena` | bump 分配器，大块预分配，O(1) 分配，批量释放（reset） |
| `StringArena` | 字符串专用 arena，内部使用 `SegmentArena` |
| `TermHashTable` / `Bucket` | 开放寻址 hash table；每个 Bucket 24B，存 term 字符串偏移、字段名偏移、TermPage 指针 |
| `TermPage` | 倒排链节点，存 doc_id 列表、词频列表，通过 `next` 串联成链 |
| `PosPage` | 位置链节点，存位置列表，通过 `next` 串联 |
| `StoredPageHeader` | 原文页头，存 doc_id → 字段值映射，通过 `next` 串联 |

`INLINE_POS_LIMIT`：position 数量超过此阈值后从 TermPage 内联区溢出到独立 PosPage 链。

## 线程安全（SWMR 模型）

- **写侧**：`IndexSearcher::attachRealtime` 以 `unique_lock<shared_mutex>` 原子替换 `rt_reader_` 指针（epoch 切换）。
- **读侧**：`search()` 以 `shared_lock<shared_mutex>` 快速复制 `shared_ptr<MemorySegmentReader>`，随后在锁外完成搜索；多线程安全。
- **`getTermMeta` 缓存**：`MemorySegmentReader` 内部的 `term_meta_cache_` 由 `mutable std::mutex term_meta_mutex_` 保护，防止多个 reader 线程并发写缓存时产生数据竞争。

## 依赖
piggy_core, piggy_analysis, piggy_field, piggy_store, piggy_index
