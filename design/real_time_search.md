# 实时索引设计文档

## 1. 目标与约束

### 目标
- 支持文档写入后**立即可查**（毫秒级可见性），无需等待 flush 到磁盘
- 实时写入与查询并发安全
- flush 到磁盘的代价尽量低（O(1) 内存回收）
- 与现有 `ISegmentReader` / `IndexSearcher` 接口无缝集成

### 约束
- C++17，不引入第三方依赖
- 单个 writer 写入，多个 reader 并发查询（SWMR）
- 内存用量可控，超阈值自动触发 flush（与现有 RAM 阈值机制一致）

---

## 2. 核心数据结构

### 2.1 页类型（Page Types）

#### TermPage（20B，固定大小）

存储一条 posting entry，代表某个 term 在某篇文档中的出现记录：

```
struct TermPage {
    uint32_t doc_id;          // 文档 ID
    uint32_t tf;              // term frequency（该 doc 中出现次数）
    uint32_t pos_head;        // 指向 PosPage 链头（tf > INLINE_POS_LIMIT 时有效）
    uint8_t  inline_pos[12];  // tf <= 3 时，positions 内嵌（3 × 4B）
    uint32_t next;            // 下一条 posting entry 的 arena offset（链表）
};  // 共 20B（tf <= 3 inline，超出才用 PosPage）
```

`inline_pos` 覆盖绝大多数低频 term（tf ≤ 3），不分配 PosPage，节省大量小对象分配。

#### PosPage（32B，固定大小）

存储 position 列表，用于 tf > 3 的情况：

```
struct PosPage {
    uint32_t positions[7];    // 最多存 7 个 position offset
    uint32_t next;            // 溢出时指向下一个 PosPage
};  // 共 32B
```

tf 较大时，多个 PosPage 通过 `next` 链接，形成 position 链。

#### StoredPage（4KB，固定大小）

存储压缩后的存储字段内容（等价于磁盘上的 `.fdt`），供查询时取回原文：

```
struct StoredPageHeader {
    uint32_t doc_id;
    uint32_t data_len;        // 实际内容长度
    uint32_t next;            // 下一个 StoredPage（大文档跨页时使用）
};
// 之后紧跟 data_len 字节的压缩内容（LZ4 或 snappy）
// 总 page 大小固定 4KB
```

---

### 2.2 SegmentArena（per-segment bump allocator）

每个写入中的内存段独占一块 arena，全部分配在其上：

```cpp
class SegmentArena {
    uint8_t* base_;         // 一次 malloc/mmap 的大块内存（如 64MB）
    size_t   capacity_;
    size_t   offset_ = 0;   // bump 指针

    uint32_t alloc(size_t size, size_t align = 4);
    void     reset();       // O(1) 回收全部，offset_ = 0
};
```

三种 page 从同一 arena 分配，按各自 size 对齐：

```
arena 内存布局示意：
[ TermPage | TermPage | PosPage | StoredPage | TermPage | PosPage | ... ]
                                    ↑ bump 指针持续右移
```

flush 后 `arena.reset()`，O(1) 回收，无 GC 压力。

---

### 2.3 TermHashTable

将 term string 映射到其倒排链表的头节点，基于**线性探测开放寻址**：

```cpp
struct Bucket {
    uint32_t term_hash;        // term 的 hash（加速比较，0 表示空）
    uint32_t str_offset;       // term string 在 StringArena 中的起始位置
    uint32_t str_len;          // term string 长度
    uint32_t term_page_head;   // 倒排链表头（TermPage 的 arena offset）
    uint32_t doc_freq;         // 当前 df（不同文档数）
    uint32_t total_tf;         // 所有文档的 tf 之和（ttf）
};  // 24B per bucket
```

初始容量根据 RAM budget 预计算，正常写入过程不触发 resize。溢出时直接 flush 而非 resize：

```
initial_capacity = ceil(ram_mb * 1024 * 1024 / avg_bytes_per_term) / 0.6
                 取最近的 2^N
```

StringArena 是独立的 bump allocator，专门存放 term string 内容：

```
StringArena: [ "search\0" | "engine\0" | "index\0" | ... ]
               ↑ str_offset
```

---

### 2.4 MemorySegment（整体结构）

```
MemorySegment（per IndexWorker）
│
├── SegmentArena（64MB，一次 malloc）
│     所有 TermPage / PosPage / StoredPage 分配于此
│
├── StringArena（8MB，一次 malloc）
│     所有 term string 存储于此
│
├── TermHashTable
│     bucket 数组（24B × capacity，从 SegmentArena 头部分配）
│     → term_string → { term_page_head, doc_freq, total_tf }
│
├── active_term_page_offset    // 当前正在写的 TermPage（per-doc 状态）
├── active_pos_page_offset     // 当前正在写的 PosPage
├── active_stored_page_offset  // 当前正在写的 StoredPage
│
└── doc_count, total_tokens    // 统计信息
```

---

## 3. 写入流程

### 3.1 单文档写入时序（无锁路径）

```
addDocument(doc):
  1. 分配 stored_page：写入压缩原文 → 得 stored_offset
  2. 对每个 field 分词，得 token list（含 positions）
  3. 对每个 term：
     a. 若 tf(term, doc) <= INLINE_POS_LIMIT（3）：
          alloc TermPage → 填 {doc_id, tf, inline_pos[...], next=old_head}
     b. 若 tf > 3：
          alloc PosPage(s) → 填 positions → 得 pos_head
          alloc TermPage  → 填 {doc_id, tf, pos_head, next=old_head}
     c. hashtable.update(term, new_term_page_offset, df++, ttf+=tf)
  4. doc_count++
```

步骤全部在 IndexWorker 私有的 MemorySegment 上操作，**写入期间零锁**。

### 3.2 两阶段写的必要性

Position 是在分词过程中逐个累积的，不能一次写完。正确顺序：

```
分词阶段（扫描 doc field tokens）：
  遇到 token "search" at pos 3 → 追加到临时 pos buffer（per-term, per-doc, 栈上）

分词完成后（单次提交）：
  for each term:
    flush temp pos buffer → alloc PosPage(s) → get pos_head
    alloc TermPage({doc_id, tf, pos_head, next=head})
    hashtable[term] = new head
```

**使用栈上临时 buffer（`std::vector<uint32_t> tmp_pos`）暂存当前 doc 中每个 term 的 positions**，doc 结束时一次性提交到 PosPage，避免半写状态进入 arena。

---

## 4. 并发模型

### 4.1 决策点：写入并发策略

**方案 A（选定）：per-writer 私有 MemorySegment，写入零锁**

```
IndexWorker 0 → MemorySegment_0（私有 arena + hashtable）
IndexWorker 1 → MemorySegment_1（私有 arena + hashtable）
...
PagePool（共享）→ 仅换页时加锁（低频）
```

| 优点 | 缺点 |
|------|------|
| 写入路径完全无锁 | 各 worker 的 hashtable 需在 flush 时合并 |
| 与现有 IndexWorker 架构天然吻合 | 同一 term 在多个 worker 中各有自己的链表 |
| 换页锁粒度极小（仅 alloc/free page）| flush 时 K-way merge term 链表 |

**方案 B（未选）：全局共享 MemorySegment，per-page 锁**

| 优点 | 缺点 |
|------|------|
| 实时查询只需访问一个 segment | 写入时 per-page 锁竞争激烈 |
| 无需 flush 后合并 hashtable | 实现复杂，易出死锁 |

选择方案 A 的理由：与现有并行架构完全一致，flush 时已有 K-way merge 逻辑可复用。

### 4.2 实时查询并发（读写共存）

实时查询需要同时访问：
- **内存 segment**（MemorySegmentReader，当前正在写入）
- **磁盘 segment**（已 flush 的 SegmentReader）

读写并发采用 **epoch-based 快照**：

```
MemorySegment 有两个状态：
  ACTIVE   → IndexWorker 正在写入（reader 可读已提交文档）
  FLUSHING → flush 中（reader 继续读，writer 切换到新 MemorySegment）

IndexSearcher 打开快照时：
  snapshot = {disk_segments[], active_memory_segment*}
  查询在 snapshot 上执行，不受之后的 flush 影响
```

flush 完成后，旧 MemorySegment 的 arena 在所有使用它的 reader 都完成后才 reset（引用计数或 hazard pointer 实现）。

---

## 5. MemorySegmentReader（ISegmentReader 实现）

新增内存版 segment reader，实现现有接口：

```cpp
class MemorySegmentReader : public ISegmentReader {
public:
    // 从 MemorySegment 构造只读视图
    explicit MemorySegmentReader(const MemorySegment& seg);

    // ISegmentReader 接口实现
    PostingIterator postingIterator(const std::string& field,
                                    const std::string& term) const override;
    const TermMeta* getTermMeta(const std::string& field,
                                 const std::string& term) const override;
    uint32_t  fieldDocLen(const std::string& field, DocId doc_id) const override;
    float     fieldAvgDocLen(const std::string& field) const override;
    uint32_t  docCount() const override;
    bool      isAlive(DocId doc_id) const override;
    StoredDoc readStoredDoc(DocId doc_id) const override;

private:
    const MemorySegment& seg_;
};
```

`postingIterator` 遍历 TermPage 链表，适配为 `PostingIterator` 接口（惰性解码）。

**IndexSearcher 修改**：构造时额外接受 `MemorySegmentReader*`（可选），查询时一并纳入：

```cpp
IndexSearcher(const std::string& dir,
              MemorySegmentReader* realtime_seg = nullptr);
```

或通过 `reload()` 动态更新内存 segment 指针。

---

## 6. PagePool（共享，低频加锁）

```cpp
class PagePool {
public:
    enum class PageType { TERM, POS, STORED };

    // 换页时调用（低频）：从 pool 取一块预分配内存
    uint8_t* allocPage(PageType type);
    // flush 后批量归还
    void recycleAll(const std::vector<uint8_t*>& pages);

private:
    std::mutex mu_;
    std::vector<uint8_t*> free_term_pages_;
    std::vector<uint8_t*> free_pos_pages_;
    std::vector<uint8_t*> free_stored_pages_;
    // 底层：大块 mmap，按 page size 切分
};
```

实际上在 per-segment arena 方案下，PagePool 只在 arena 满时交互（分配新的大块内存），平时写入操作完全不接触 PagePool。

---

## 7. Flush 流程

```
flush(memory_segment → disk_segment_id):

  Phase 1：倒排索引序列化
    for each term in hashtable:
      traverse TermPage 链表 → 收集 {doc_id, tf, positions}
      按 doc_id 排序（链表头部是最新写入，顺序未必有序）
      PForDelta 编码 → 写 .doc_<field>
      positions → 写 .pos_<field>
      TermMeta → 写 .tim_<field>

  Phase 2：存储字段序列化
    traverse StoredPage 链 → 写 .fdt / .fdx

  Phase 3：FastField（数值列存）
    遍历每个 doc 的 ff 字段 → 写 .ff_<field>

  Phase 4：清理
    arena.reset()         // O(1)
    hashtable.clear()     // O(capacity)，可进一步优化为 swap 空 table
    doc_count = 0

  Phase 5：磁盘标记
    创建 segment_{id}/.done
```

---

## 8. 关键决策点汇总

| 决策点 | 选定方案 | 优点 | 缺点 |
|--------|---------|------|------|
| Arena 类型 | per-segment bump allocator | flush 后 O(1) reset；实现简单 | 段内无法单独释放某个对象 |
| Position 存储 | tf≤3 inline TermPage，超出 PosPage 链 | 绝大多数场景零额外分配 | tf>3 需要额外链一个 PosPage |
| 写入并发 | per-worker 私有 MemorySegment | 写入零锁 | flush 时需合并多个 hashtable |
| 读写共存 | epoch 快照 + MemorySegmentReader | 查询不阻塞写入 | 需要引用计数管理 arena 生命周期 |
| HashTable | 预分配线性探测，溢出触发 flush | 写入 O(1)，无 resize | 容量估算不准时提前 flush |
| PagePool 加锁 | 只在换页（arena 耗尽）时加锁 | 正常写入路径零锁 | 极端情况频繁换页时短暂竞争 |
| 实时可见性 | IndexSearcher 接受 MemorySegmentReader | 复用现有查询链路 | Searcher 需要定期 reload 以感知新内存 segment |

---

## 9. 改造步骤

### Step 1：SegmentArena + 三种 Page 结构
**文件**
```
modules/memory/
  include/memory/
    segment_arena.h      ← SegmentArena, bump allocator
    term_page.h          ← TermPage / PosPage / StoredPage 结构定义
  src/
    segment_arena.cpp
  CMakeLists.txt
```
**验证**：单元测试验证 alloc/reset 正确性，内存不越界，reset 后可复用。

---

### Step 2：TermHashTable
**文件**
```
modules/memory/include/memory/term_hash_table.h
modules/memory/src/term_hash_table.cpp
```
**关键接口**
```cpp
class TermHashTable {
    void     insert(std::string_view term, uint32_t term_page_offset, uint32_t tf);
    uint32_t lookup(std::string_view term) const;  // 返回 head offset，未找到返回 INVALID
    void     clear();
    float    loadFactor() const;
};
```
**验证**：10 万词条插入查询正确；load factor 0.75 时行为；hash 冲突均匀分布。

---

### Step 3：MemorySegment（写入核心）
**文件**
```
modules/memory/include/memory/memory_segment.h
modules/memory/src/memory_segment.cpp
```
**关键接口**
```cpp
class MemorySegment {
    void addDocument(const Document& doc,
                     const std::vector<FieldDescriptor*>& descs,
                     DocId doc_id);
    size_t ramUsed() const;
    uint32_t docCount() const;
};
```
**验证**：写入 1000 篇文档后，hashtable 中每个 term 的 df/ttf 与单线程 IndexWriter 完全一致。

---

### Step 4：MemorySegmentReader（ISegmentReader 实现）
**文件**
```
modules/memory/include/memory/memory_segment_reader.h
modules/memory/src/memory_segment_reader.cpp
```
**验证**：通过 MemorySegmentReader 搜索，结果与从相同文档 flush 后的磁盘 SegmentReader 搜索结果一致（term df / posting list 顺序相同）。

---

### Step 5：Flush 路径（MemorySegment → 磁盘）
**文件**：在 `modules/memory/src/memory_segment.cpp` 中实现 `flushToDisk(dir, seg_id)`

**核心挑战**：TermPage 链表写入顺序是反序（最新 doc 在链头），flush 时需要按 doc_id 排序：
- 遍历链表收集所有 (doc_id, tf, positions)
- std::sort by doc_id
- PForDelta 编码写磁盘

**验证**：flush 后 `SegmentReader` 打开，搜索结果与 flush 前 `MemorySegmentReader` 搜索结果完全一致。

---

### Step 6：IndexSearcher 集成实时 Segment
**文件**：修改 `modules/query/src/index_searcher.cpp`

```cpp
class IndexSearcher {
    // 新增：绑定实时 segment
    void attachRealtime(const MemorySegmentReader* rt);
    void detachRealtime();
    // 查询时自动将 rt segment 纳入打分
};
```

**验证**：写入文档后不 flush，直接搜索命中；flush 后 reload 磁盘 segment，搜索结果不变；实时 segment + 磁盘 segment 混合查询结果与单独查询之和一致。

---

### Step 7：并发安全（SWMR）
**文件**：在 MemorySegment / MemorySegmentReader 之间加入引用计数快照机制

**验证**（压力测试）：4 线程写入，4 线程查询，持续 10 秒；无数据竞争（TSAN 检测）；doc_count 精确匹配写入总数；查询命中数单调不减（写入越多命中越多）。

---

## 10. 测试计划

| 测试文件 | 对应 Step | 验证维度 |
|----------|-----------|---------|
| `test_segment_arena.cpp` | Step 1 | alloc 对齐正确；reset 后复用；越界检测 |
| `test_term_hash_table.cpp` | Step 2 | 插入/查询/hash 冲突；load factor；clear 后复用 |
| `test_memory_segment_write.cpp` | Step 3 | df/ttf 与单线程参考一致；inline pos 与 PosPage 路径均正确 |
| `test_memory_segment_reader.cpp` | Step 4 | postingIterator 遍历顺序；getTermMeta 正确；readStoredDoc 内容完整 |
| `test_memory_flush.cpp` | Step 5 | flush 前后查询结果一致；flush 后 arena reset，doc_count 归零 |
| `test_realtime_searcher.cpp` | Step 6 | 写入即可查；混合查询（内存+磁盘）结果正确；reload 后不丢失文档 |
| `test_realtime_concurrent.cpp` | Step 7 | SWMR 无数据竞争（TSAN）；doc_count 精确；查询单调不减 |

---

## 11. 文件结构

```
modules/
  memory/                          ← 新模块
    CMakeLists.txt
    CLAUDE.md
    include/memory/
      segment_arena.h              ← Step 1
      term_page.h                  ← Step 1
      term_hash_table.h            ← Step 2
      memory_segment.h             ← Step 3
      memory_segment_reader.h      ← Step 4
    src/
      segment_arena.cpp
      term_hash_table.cpp
      memory_segment.cpp           ← Step 3 + 5（含 flushToDisk）
      memory_segment_reader.cpp

  query/
    src/index_searcher.cpp         ← Step 6（新增 attachRealtime）

tests/
  memory/
    test_segment_arena.cpp
    test_term_hash_table.cpp
    test_memory_segment_write.cpp
    test_memory_segment_reader.cpp
    test_memory_flush.cpp
    test_realtime_searcher.cpp
    test_realtime_concurrent.cpp
```

---

## 12. 模块依赖

```
core ◄── memory ◄── query
          │
          ├── analysis  （分词）
          ├── codec     （PForDelta 编码，flush 时使用）
          ├── field     （FastField、StoredDoc）
          └── index     （SegmentWriter，flush 时复用写磁盘逻辑）
```

`memory` 模块对 `query` 的依赖仅通过 `ISegmentReader` 接口，不反向依赖 `IndexSearcher`。

---

## 13. 退出安全与崩溃恢复

### 13.1 正常退出（Graceful Shutdown）

内存中未 flush 的数据必须在进程退出前落盘，分两个触发层：

**层一：RAII 析构 flush**

`MemorySegment` 析构时，若 `doc_count_ > 0` 则自动触发 `flushToDisk()`，类比现有 `ParallelIndexWriter` 的析构行为：

```cpp
MemorySegment::~MemorySegment() {
    if (doc_count_ > 0 && !flushed_) {
        try { flushToDisk(dir_, nextSegId()); } catch (...) {}
    }
}
```

**层二：信号处理**

进程收到 `SIGTERM` / `SIGINT` 时，单纯依赖 RAII 不够可靠（全局对象析构顺序不确定）。需要在应用层注册信号处理函数，显式调用 flush：

```cpp
// 在 RealtimeIndexWriter 初始化时注册
static RealtimeIndexWriter* g_writer = nullptr;

signal(SIGTERM, [](int) { if (g_writer) g_writer->flush(); std::exit(0); });
signal(SIGINT,  [](int) { if (g_writer) g_writer->flush(); std::exit(0); });
```

**flush 完成标志**：flush 结束后写入 `segment_{id}/.done`，与现有约定一致。正常退出路径保证不产生孤儿 `.ing` 文件。

---

### 13.2 异常退出（Crash Recovery）

进程 crash 时，内存数据无法通过 RAII 保护，取决于是否引入 WAL。

#### 决策点：是否引入 WAL

| | 方案 A：无 WAL，接受数据丢失 | 方案 B：WAL，保证持久性 |
|--|--|--|
| 实现复杂度 | 低 | 高（需实现 log 写入、replay、截断） |
| 数据丢失 | crash 丢失上次 flush 后的所有写入 | 最多丢失最后一个 WAL 写入（可配置 sync 频率）|
| 写入延迟 | 低（仅内存操作）| 略高（每条文档额外写一次 WAL）|
| 适用场景 | 可重建索引的场景（如 wiki 全量重建）| 增量写入、数据不可再生的场景 |

**本系统初期选择方案 A**，理由：
- wiki_indexer 数据源完整，crash 后重建代价可接受
- 实时索引主要用于增量补充，大量数据已在磁盘 segment 中
- WAL 可作为后续扩展，接口预留

---

### 13.3 启动时 Recovery 扫描

无论有无 WAL，启动时都需要清理上次异常退出的残留状态：

```
startup recovery 流程：
  1. 扫描 index_dir/ 下所有 segment_{id}/ 子目录
  2. 有 .done 无 .ing  → 完整 segment，正常加载
  3. 有 .ing  无 .done → 写入中途 crash，残留不完整 segment
     → 删除整个 segment_{id}/ 目录（rm -rf）
  4. 有 .ing  有 .done → 不应出现（flush 最后先写 .done 再删 .ing）
                          若出现则视为完整 segment（.done 已生成）

// 实现位置：IndexSearcher 构造时已扫描 .done，天然跳过不完整 segment
// 额外步骤：清理孤儿 .ing 目录（可选，不清理也不影响正确性，只是占磁盘）
```

清理函数：

```cpp
// 启动时调用，清理残留的不完整 segment 目录
void cleanupIncompleteSegments(const std::string& dir) {
    for (const auto& e : fs::directory_iterator(dir)) {
        if (!e.is_directory()) continue;
        auto name = e.path().filename().string();
        if (name.substr(0, 8) != "segment_") continue;
        bool has_done = fs::exists(e.path() / ".done");
        bool has_ing  = fs::exists(e.path() / ".ing");
        if (has_ing && !has_done) {
            std::cerr << "[Recovery] Removing incomplete segment: " << name << "\n";
            fs::remove_all(e.path());
        }
    }
}
```

---

### 13.4 Checkpoint 策略（flush 频率控制）

即使不用 WAL，定期 flush 也能限制 crash 后的数据丢失窗口：

```
flush 触发条件（任一满足即触发）：
  1. ramUsed() >= ram_threshold      ← 已有，内存压力
  2. doc_count_ >= doc_threshold     ← 新增，如每 10 万篇文档 flush 一次
  3. time_since_last_flush >= T      ← 新增，如每 60 秒 flush 一次（时间窗口兜底）
  4. 进程退出信号                    ← 新增，SIGTERM/SIGINT
  5. 显式调用 flush()               ← 已有
```

时间窗口兜底（条件 3）需要后台定时线程或在 `addDocument` 中检查时间戳（后者更简单，无额外线程）：

```cpp
void MemorySegment::addDocument(...) {
    // ... 正常写入 ...
    if (shouldFlush()) triggerFlush();  // 检查所有触发条件
}

bool MemorySegment::shouldFlush() const {
    return ramUsed() >= ram_threshold_
        || doc_count_ >= doc_threshold_
        || (Clock::now() - last_flush_time_) >= flush_interval_;
}
```

---

### 13.5 测试补充

在测试计划基础上新增两项：

| 测试文件 | 验证维度 |
|----------|---------|
| `test_graceful_shutdown.cpp` | 析构触发 flush，磁盘出现完整 `.done` segment；signal 路径下文档不丢失 |
| `test_crash_recovery.cpp` | 模拟 crash（直接 exit(1) 跳过析构）后，启动时 `.ing` 残留被清理，已 flush 的 `.done` segment 完整加载 |

---

## 14. 已知风险与缓解

| 风险 | 缓解 |
|------|------|
| TermPage 链表 flush 时排序开销 | doc_id 单调递增，若写入有序则链表已近乎有序，sort 接近 O(N) |
| Arena 容量估算不准触发提前 flush | 监控 ramUsed()，在 90% 时预警，给调用方调整 RAM 预算的机会 |
| 大文档 StoredPage 跨多页 | StoredPage header 中记录 next，flush 时拼接；单页 4KB 足以覆盖大多数文档 |
| SWMR 下 MemorySegmentReader 访问已 reset 的 arena | 引用计数：reader 持有 arena 的 shared_ptr，flush 等待引用计数为 0 后再 reset |
| 实时场景 hash 分布极端不均（所有词同 hash）| 使用 MurmurHash3，分布均匀；监控最长探测链长度 |
| 正常退出时 flush 失败（磁盘满、IO 错误）| 析构中 catch 所有异常并打印日志；提供 `flush()` 返回值供调用方检查 |
| SIGKILL 无法捕获 | 不可抗力；靠定期 checkpoint（条件 3）缩小丢失窗口；WAL 是最终解法 |
