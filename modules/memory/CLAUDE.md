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

## .fdt 写入不变式（与 index/SegmentMerger 的隐式契约）

**每个 doc 必须写入 fdt，即使所有字段 `stored=false`。**

格式：`[doc_id:4B][ext_id:8B][n_fields:4B=0]`

### 为什么必须这样

`SegmentMerger::doMerge` 的 remap 构建依赖 `readStoredDoc(local).doc_id` 来还原文档在 posting list 中的原始 doc_id：

```
remap_key = (seg_id << 20) | stored.doc_id   ← 来自 fdt
lambda_key = (seg_id << 20) | iter.docId()   ← 来自 .pos_<field>
```

两侧必须相等，remap 查找才能命中。若 fdt 没有该 doc 的记录，`readStoredDoc` 返回 `doc_id=0`，remap_key ≠ lambda_key，合并后该 doc 对所有 term 都消失，产生 **"N docs but 0 terms"** 的 silent data loss。

**ext_id 同理**：`IndexSearcher::mergeTopK` 的去重条件是 `ext_id != 0`；fdt 缺失导致 `ext_id=0`，相同 ext_id 的文档无法去重。

### 排查路径（历史教训）

症状在 merger 层出现，根因在 memory 层的写入逻辑，中间还走了一条错误方向：

1. **症状**：`test_flush_worker::three_entry_kway_merge` — 合并后 segment 有 3 docs、`tim_body` 却只有 4B（term_count=0）
2. **误导信息**：merger 日志输出 `Field "body": 6 terms merged`，说明 field_terms 收集是对的；坏的是 kway merge 本身对每个 term 都产生空 merged 列表
3. **加 debug 打印**：remap build 侧输出 `stored.doc_id=0`；lambda 侧输出 `docId()=1` → 两侧 key 差 1，remap.find() 全部 miss
4. **第一次错误修复**：把 remap key 改为 `| local`（1-indexed 位置）代替 `| stored.doc_id`，通过了 RT 测试，但 `test_merger` 立即挂掉
5. **原因**：`SegmentWriter` 允许 doc 有全局 doc_id（如 3），segment 内 `dc=1` 但 posting list 中存的是 `doc_id=3`；`local=1 ≠ 3`，remap miss 倒过来了
6. **正确定位**：根因是 `writeStoredFields` 在 `n_fields==0` 时 early return，不写 `stored_heads_`，导致 fdt 跳过该 doc，fdx 里没有对应条目
7. **正确修复**：在 fdt 写入循环中，先写 `doc_id + ext_id`，再判断是否有 stored fields；无论如何都写入 fdx 条目

**关键教训**：看到 merger 说"N terms merged"但输出 0 terms，优先怀疑 remap key 不匹配，用 `cerr` 同时打印 remap build 侧和 lambda 侧的 key，立刻能定位。

## 依赖
piggy_core, piggy_analysis, piggy_field, piggy_store, piggy_index
