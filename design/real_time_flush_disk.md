# 实时索引异步 Flush 设计文档

> 补充 `real_time_search.md`（Step 1-7 基础实现）。  
> 本文档覆盖：多实时线程 freeze → 异步 flush → k-way merge → 原子提交的完整机制。

---

## 1. 目标与约束

### 目标
- 实时 doc 从 `addDocument` 到可被检索，全链路（内存 → flush → 磁盘加载）零可见性中断
- 实时写入线程在 flush 过程中**不阻塞**，由独立异步线程完成所有磁盘 IO
- k-way merge 批量 flush，减少小 segment 数量，同时限制峰值内存占用
- 每个 frozen segment 全生命周期可监控

### 约束（已确认）
- 每个实时写入线程持有独立 `MemorySegment`（写入零锁）
- freeze 触发：`ramUsed()` 超阈值 **或** TermHashTable 利用率超阈值，任一满足即触发
- flush 默认成功；失败时 frozen reader 保留在 reader list，不影响可见性
- double-count 窗口（add disk reader 后、remove frozen reader 前）通过 `ext_id` 去重解决

---

## 2. 整体组件图

```
┌──────────────────────────────────────────────────────────────────────────┐
│  IndexSearcher                                                            │
│                                                                           │
│  all_readers_: vector<shared_ptr<ISegmentReader>>                        │
│  ┌───────────┬───────────┬───────────────┬──────────────────────────┐   │
│  │DiskReader │DiskReader │FrozenMemReader│ ActiveMemReader[thread 0] │   │
│  │    0      │    1      │   [seg 2]     │ ActiveMemReader[thread 1] │   │
│  │           │           │FrozenMemReader│ ActiveMemReader[thread 2] │   │
│  │           │           │   [seg 3]     │                          │   │
│  └───────────┴───────────┴───────────────┴──────────────────────────┘   │
│                                                                           │
│  readers_mutex_: shared_mutex                                             │
│  addReader(reader)                                                        │
│  commitDiskSegment(new_disk, {frozen_reader...})  ← 原子 add + remove   │
│  removeReader(reader)                                                     │
│  search() → copy all_readers_ → 搜索 → ext_id 去重 → 返回 TopK          │
└────────────────────────────────▲────────────────────────────────────────┘
                                 │ commitDiskSegment()
                  ┌──────────────┴──────────────┐
                  │  FlushWorker                 │  (1-2 个线程)
                  │                              │
                  │  1. takeBatch(max=3, 500ms)  │
                  │  2. entry.seg->flushToDisk() │  ← 每个独立写 tmp segment
                  │  3. SegmentMerger k-way      │  ← batch≥2 时合并
                  │  4. load SegmentReader       │
                  │  5. commitDiskSegment()      │
                  │  6. update FrozenSegStats    │
                  └──────────────▲──────────────┘
                                 │ push(FrozenEntry)
                  ┌──────────────┴──────────────┐
                  │  FlushQueue                  │
                  │  deque<FrozenEntry>          │
                  │  mutex + condvar             │
                  │  takeBatch(max, timeout)     │
                  └──────────────▲──────────────┘
                                 │ freeze()
       ┌─────────────────────────┼─────────────────────────┐
       │                         │                         │
  RTIndexThread[0]         RTIndexThread[1]         RTIndexThread[2]
  MemorySegment[0]         MemorySegment[1]         MemorySegment[2]
  checkAndFreeze()         checkAndFreeze()         checkAndFreeze()
  (每次 addDocument 后)
```

---

## 3. 接口改造

### 3.1 IndexSearcher

```cpp
// ── 现有（删除）────────────────────────────────────────────────────────
std::shared_ptr<MemorySegmentReader> rt_reader_;
void attachRealtime(shared_ptr<MemorySegmentReader>);
void detachRealtime();

// ── 新增 ───────────────────────────────────────────────────────────────
// 全部 reader（disk + frozen memory + active memory）统一管理
std::vector<std::shared_ptr<ISegmentReader>> all_readers_;
std::shared_mutex readers_mutex_;

// RT 线程 startup 或 segment swap 时调用（注册新的 active reader）
void addReader(std::shared_ptr<ISegmentReader> reader);

// FlushWorker 完成 flush 后调用：
//   原子操作：先把 new_disk 加入 all_readers_，再移除 frozen_to_remove
//   保证任意时刻 doc 均可见（不出现空窗）
void commitDiskSegment(
    std::shared_ptr<ISegmentReader>              new_disk,
    std::vector<std::shared_ptr<ISegmentReader>> frozen_to_remove);

// RT 线程 shutdown 时调用
void removeReader(std::shared_ptr<ISegmentReader> reader);
```

**search() 去重逻辑**：

```cpp
// 搜完所有 reader 后，mergeTopK 中增加一个 ext_id 去重 pass：
// 相同 ext_id 保留 score 最高的那条，其余丢弃。
// ext_id 已在 SearchResult 中（r.ext_id = stored.ext_id），无需新增接口。
std::unordered_map<uint64_t, SearchResult> dedup;
for (auto& r : all_hits) {
    auto [it, inserted] = dedup.emplace(r.ext_id, r);
    if (!inserted && r.score > it->second.score)
        it->second = r;
}
```

去重窗口极短（`unique_lock` 内完成 add + remove），实际命中重复的概率很低；
正确性由去重兜底保证，不依赖时序。

---

### 3.2 新增组件接口

#### FrozenEntry & FrozenSegStats

```cpp
enum class FrozenSegState {
    Queued,       // 进入 FlushQueue，等待 FlushWorker 取走
    Flushing,     // FlushWorker 正在 flushToDisk
    OnDisk,       // tmp segment 已写完，等待 merge
    Merging,      // SegmentMerger 处理中
    Committed,    // disk reader 已进入 all_readers_，frozen reader 已移除
};

struct FrozenSegStats {
    uint32_t thread_id;
    uint32_t seg_id;
    uint32_t doc_count;
    uint64_t ram_bytes;
    float    hash_utilization;
    FrozenSegState state;

    std::chrono::steady_clock::time_point freeze_time;
    std::chrono::steady_clock::time_point queue_time;
    std::chrono::steady_clock::time_point flush_start_time;
    std::chrono::steady_clock::time_point on_disk_time;
    std::chrono::steady_clock::time_point merge_start_time;
    std::chrono::steady_clock::time_point committed_time;
    // memory release：MemorySegment 析构时（shared_ptr 引用计数归零）自动记录
};

struct FrozenEntry {
    std::shared_ptr<MemorySegment>       segment;
    std::shared_ptr<ISegmentReader>      frozen_reader;  // 待从 all_readers_ 移除
    std::shared_ptr<FrozenSegStats>      stats;
};
```

#### FlushQueue

```cpp
class FlushQueue {
public:
    void push(FrozenEntry entry);

    // 等待直到 size >= max_batch 或超时 timeout_ms，取走 min(size, max_batch) 个
    std::vector<FrozenEntry> takeBatch(int max_batch, int timeout_ms);

    size_t size() const;
};
```

#### FlushWorker

```cpp
class FlushWorker {
public:
    FlushWorker(const std::string& dir, const Schema& schema,
                FlushQueue& queue, IndexSearcher& searcher,
                std::atomic<uint32_t>& global_seg_id);

    void start();   // 启动后台线程
    void stop();    // 等待当前 batch 完成后退出

private:
    void run();

    // run() 主循环：
    //   1. batch = queue_.takeBatch(3, 500)
    //   2. for each entry: flushToDisk(tmp_id)       → state = OnDisk
    //   3. if batch.size() == 1: final_id = tmp_id
    //      else: SegmentMerger({tmp_ids}).mergeAll(final_id) → state = Merging → Committed
    //            删除 tmp segments
    //   4. disk_reader = make_shared<SegmentReader>(dir, final_id)
    //   5. searcher_.commitDiskSegment(disk_reader, {entry.frozen_reader...})
    //   6. 打点 committed_time
};
```

#### RTIndexThread

```cpp
struct FreezeConfig {
    size_t ram_bytes_threshold;       // 主触发：e.g. 256MB
    float  hash_utilization_threshold; // 次触发：e.g. 0.75
};

class RTIndexThread {
public:
    RTIndexThread(uint32_t thread_id, const std::string& dir,
                  const Schema& schema, IndexSearcher& searcher,
                  FlushQueue& queue, std::atomic<uint32_t>& global_seg_id,
                  FreezeConfig cfg);

    void addDocument(const Document& doc);

    // 线程结束时显式 freeze，确保剩余文档进入 flush 队列
    void shutdown();

private:
    void checkAndFreeze();

    bool shouldFreeze() const {
        return active_seg_->ramUsed()             >= cfg_.ram_bytes_threshold ||
               active_seg_->termHashUtilization() >= cfg_.hash_utilization_threshold;
    }

    void freeze();
    // freeze() 步骤：
    //   1. new_seg    = make_shared<MemorySegment>(...)
    //   2. new_id     = global_seg_id_.fetch_add(1)
    //   3. new_reader = make_shared<MemorySegmentReader>(new_seg, new_id, schema_)
    //   4. searcher_.addReader(new_reader)          ← 新 active reader 先进 all_readers_
    //   5. queue_.push({active_seg_, active_reader_, stats})
    //   6. active_seg_ = new_seg; active_reader_ = new_reader
};
```

---

## 4. 完整数据流

### 正常写入 → freeze → flush → 提交

```
[RT Thread]
  addDocument(doc)
    → active_seg_->addDocument(doc)
    → checkAndFreeze()
        shouldFreeze? NO  → 继续
        shouldFreeze? YES → freeze()
            new_seg = new MemorySegment
            new_reader = new MemorySegmentReader(new_seg)
            searcher.addReader(new_reader)      // all_readers_ 追加 active reader
            queue.push({old_seg, old_reader})   // old_reader 仍在 all_readers_
            active = new_seg / new_reader

[FlushWorker]
  batch = queue.takeBatch(max=3, timeout=500ms)
  for each entry in batch:
    entry.stats->state = Flushing
    tmp_id = global_seg_id++
    entry.seg->flushToDisk(dir, tmp_id, schema)   // MemorySegment → segment_N/
    entry.stats->state = OnDisk

  if batch.size() == 1:
    final_id = tmp_id
  else:
    entry.stats->state = Merging
    SegmentMerger(dir, {tmp_ids}).mergeAll(final_id)
    for each tmp_id: fs::remove_all("segment_" + tmp_id)

  disk_reader = make_shared<SegmentReader>(dir, final_id)

  searcher.commitDiskSegment(disk_reader, {entry.frozen_reader...})
    → unique_lock<shared_mutex>
        all_readers_.push_back(disk_reader)           // 先加磁盘
        erase frozen_readers from all_readers_         // 再移内存
    → unlock

  for each entry: entry.stats->state = Committed; committed_time = now()

[MemorySegment 析构]
  当 frozen_reader shared_ptr 引用计数归零（all_readers_ 中已移除，
  所有 in-flight search 也已完成），MemorySegment 析构，arena 释放
```

### 搜索路径（并发安全）

```
[Search Thread]
  {
    shared_lock lock(readers_mutex_)
    auto readers = all_readers_   // 快速 copy shared_ptr vector
  }                               // 锁在此释放，后续搜索完全在锁外进行

  for each reader in readers:
    hits += reader->search(query)   // disk reader 或 memory reader，接口统一

  all_hits = mergeTopK(hits)
  dedup by ext_id (keep highest score per ext_id)
  return top_k results
```

---

## 5. FlushQueue 批量策略

```
takeBatch(max_batch=3, timeout_ms=500):
  wait_until:
    queue.size() >= max_batch   → 立即取走，不再等待（防止堆积）
    OR elapsed >= timeout_ms    → 取走当前所有（防止低流量时数据滞留内存）

  取 min(queue.size(), max_batch) 个 entry
```

策略效果：
- 高流量：多个 thread 并发 freeze → 队列快速积累到 3 → 立即触发 k-way merge
- 低流量：最多等 500ms，即使只有 1 个 frozen segment 也会被 flush

---

## 6. Segment ID 分配

所有 MemorySegment（active/frozen）和 disk segment 共享一个全局原子计数器：

```cpp
std::atomic<uint32_t> global_seg_id{0};

uint32_t nextSegId() { return global_seg_id.fetch_add(1); }
```

- RTIndexThread 创建新 MemorySegment 时取一个 ID（active reader 注册用）
- FlushWorker 写 tmp segment 和 final segment 时各取一个 ID
- ID 全局单调递增，all_readers_ 中不同 reader 的 seg_id 不重复

---

## 7. 可见性保证

| 时刻 | Doc 在哪个 Reader 里 |
|------|---------------------|
| addDocument 后 | active MemorySegmentReader（已在 all_readers_） |
| freeze 后，flush 前 | frozen MemorySegmentReader（仍在 all_readers_） |
| flushToDisk 进行中 | frozen MemorySegmentReader（仍在 all_readers_） |
| commitDiskSegment 执行瞬间 | disk reader 和 frozen reader **同时**在 all_readers_ |
| commitDiskSegment 完成后 | disk SegmentReader（frozen reader 已从 all_readers_ 移除） |

double-count 窗口 = `unique_lock` 持有期间（microseconds 级别），极短。  
ext_id 去重在搜索结果合并时处理，逻辑正确性不依赖窗口长度。

---

## 8. Flush 失败处理（当前阶段）

默认 flush 成功。若 `flushToDisk` 或 `SegmentMerger` 抛出异常：

- `frozen_reader` **不从** `all_readers_` 中移除（数据不丢失，仍可查询）
- 打印错误日志，`FrozenSegStats.state` 保留 `OnDisk` 或 `Merging`
- FlushWorker 继续处理下一个 batch（不阻塞整个 flush 链路）
- 后续版本可加重试队列

---

## 9. 改造步骤

### Step 1：IndexSearcher 接口改造

**文件**
```
modules/query/include/query/index_searcher.h
modules/query/src/index_searcher.cpp
```

**改动**
```cpp
// 删除
std::shared_ptr<MemorySegmentReader> rt_reader_;
void attachRealtime(shared_ptr<MemorySegmentReader>);
void detachRealtime();

// 新增
std::vector<std::shared_ptr<ISegmentReader>> all_readers_;
std::shared_mutex readers_mutex_;

void addReader(std::shared_ptr<ISegmentReader>);

void commitDiskSegment(
    std::shared_ptr<ISegmentReader>              new_disk,
    std::vector<std::shared_ptr<ISegmentReader>> frozen_to_remove);

void removeReader(std::shared_ptr<ISegmentReader>);
```

`search()` 改动：`shared_lock` 仅用于 copy `all_readers_`，锁外完成所有搜索；`mergeTopK` 末尾增加 ext_id 去重 pass（`SearchResult.ext_id` 已有，无需新增接口）。

**验证**：现有全部测试不回归；对同一 reader 并发调用 `addReader` / `removeReader` / `search` 无数据竞争（TSAN）；`commitDiskSegment` 完成后 `all_readers_` 中恰好出现 new_disk、消失 frozen_to_remove。

---

### Step 2：FrozenSegStats + FrozenEntry 定义

**文件**
```
modules/memory/include/memory/frozen_seg_stats.h   （header-only）
```

**内容**
```cpp
enum class FrozenSegState { Queued, Flushing, OnDisk, Merging, Committed };

struct FrozenSegStats {
    uint32_t thread_id, seg_id, doc_count;
    uint64_t ram_bytes;
    float    hash_utilization;
    FrozenSegState state;

    steady_clock::time_point freeze_time, queue_time,
                             flush_start_time, on_disk_time,
                             merge_start_time, committed_time;
};

struct FrozenEntry {
    std::shared_ptr<MemorySegment>  segment;
    std::shared_ptr<ISegmentReader> frozen_reader;
    std::shared_ptr<FrozenSegStats> stats;
};
```

**验证**：state 枚举值可用 `<<` 打印（便于日志）；`FrozenSegStats` 各 time_point 默认为 `time_point{}`（未打点状态可区分）。

---

### Step 3：FlushQueue

**文件**
```
modules/memory/include/memory/flush_queue.h
modules/memory/src/flush_queue.cpp
```

**关键接口**
```cpp
class FlushQueue {
public:
    void push(FrozenEntry entry);

    // 等待直到 size >= max_batch 或超时 timeout_ms，取走 min(size, max_batch) 个
    std::vector<FrozenEntry> takeBatch(int max_batch, int timeout_ms);

    size_t size() const;

    // 通知所有等待的 takeBatch 立即返回（用于 shutdown）
    void shutdown();

private:
    std::deque<FrozenEntry>   queue_;
    mutable std::mutex        mu_;
    std::condition_variable   cv_;
    bool                      shutdown_ = false;
};
```

**验证**：单线程 push 3 个 → `takeBatch(3, 1000)` 立即返回 3 个；push 1 个 → `takeBatch(3, 100)` 超时后返回 1 个；4 线程并发 push 100 个 → `takeBatch` 累计取到 100 个，无丢失；`shutdown()` 后 `takeBatch` 立即返回空。

---

### Step 4：FlushWorker

**文件**
```
modules/memory/include/memory/flush_worker.h
modules/memory/src/flush_worker.cpp
```

**关键接口**
```cpp
class FlushWorker {
public:
    FlushWorker(const std::string& dir, const Schema& schema,
                FlushQueue& queue, IndexSearcher& searcher,
                std::atomic<uint32_t>& global_seg_id);

    void start();  // 启动后台线程
    void stop();   // 等待当前 batch 完成后退出
};
```

**主循环逻辑**
```
run():
  while (!stop_requested_):
    batch = queue_.takeBatch(3, 500)
    if batch.empty(): continue

    for each entry:
      stats->state = Flushing; flush_start_time = now()
      tmp_id = global_seg_id_++
      entry.seg->flushToDisk(dir_, tmp_id, schema_)
      stats->state = OnDisk; on_disk_time = now()

    if batch.size() == 1:
      final_id = tmp_ids[0]
    else:
      stats->state = Merging; merge_start_time = now()
      SegmentMerger(dir_, {tmp_ids}).mergeAll(final_id)
      for tmp_id in tmp_ids: fs::remove_all("segment_" + tmp_id)

    disk_reader = make_shared<SegmentReader>(dir_, final_id)
    searcher_.commitDiskSegment(disk_reader, {entry.frozen_reader...})
    stats->state = Committed; committed_time = now()
```

**验证**：写入 1 个 frozen segment → FlushWorker 产出磁盘可读的 SegmentReader，doc_count 正确；写入 3 个 frozen segment → k-way merge 后 doc_count = 三者之和，term df 等于各 segment 之和；`commitDiskSegment` 调用后对应 frozen_reader 已从 `all_readers_` 中消失；`stop()` 后后台线程正常退出。

---

### Step 5：RTIndexThread

**文件**
```
modules/memory/include/memory/rt_index_thread.h
modules/memory/src/rt_index_thread.cpp
```

**关键接口**
```cpp
struct FreezeConfig {
    size_t ram_bytes_threshold;        // 主触发：e.g. 256 * 1024 * 1024
    float  hash_utilization_threshold; // 次触发：e.g. 0.75f
};

class RTIndexThread {
public:
    RTIndexThread(uint32_t thread_id,
                  const std::string& dir, const Schema& schema,
                  IndexSearcher& searcher, FlushQueue& queue,
                  std::atomic<uint32_t>& global_seg_id,
                  FreezeConfig cfg);

    void addDocument(const Document& doc);

    // 线程结束前调用：将剩余文档 freeze 进 FlushQueue
    void shutdown();

private:
    bool shouldFreeze() const;
    void freeze();   // 原子 swap：addReader(new) → push(old) → 切换 active
};
```

**freeze() 保证**：`addReader(new_reader)` 在 `push(old_entry)` 之前调用，保证新 active reader 进入 `all_readers_` 后，old reader 才进入 FlushQueue，旧文档始终可查。

**验证**：ramUsed 超阈值触发 freeze，触发后旧 reader 仍在 `all_readers_`；freeze 后新 segment 立即接受写入；`shutdown()` 后剩余文档进入 FlushQueue，doc_count 与写入数一致；多线程并发调用 `addDocument` + `shutdown` 无数据竞争（每个线程独立，无锁）。

---

### Step 6：端到端集成测试

**文件**
```
tests/memory/test_rt_flush_e2e.cpp
```

**场景**
```
1. 启动 global_seg_id 计数器、FlushQueue、FlushWorker
2. 启动 N 个 RTIndexThread（N=3）
3. 主线程持续调用 search()（异步，独立线程）
4. 各 RT 线程写入足量文档触发 2+ 次 freeze
5. shutdown() 所有 RT 线程 → FlushWorker stop()
6. 验证：
   a. 全程搜索无空窗（特定 ext_id 在 addDocument 后 search 始终可命中）
   b. ext_id 去重：最终结果无重复 ext_id
   c. 磁盘 segment 的 doc_count 总和 = 各线程写入文档数之和
   d. FlushWorker 停止后 MemorySegment 无内存泄漏（shared_ptr 计数归零）
```

---

## 10. 文件结构

```
modules/
  memory/
    include/memory/
      frozen_seg_stats.h     ← Step 2（header-only）
      flush_queue.h          ← Step 3
      flush_worker.h         ← Step 4
      rt_index_thread.h      ← Step 5
    src/
      flush_queue.cpp        ← Step 3
      flush_worker.cpp       ← Step 4
      rt_index_thread.cpp    ← Step 5
    CMakeLists.txt           ← 追加新源文件

  query/
    include/query/
      index_searcher.h       ← Step 1（接口变更）
    src/
      index_searcher.cpp     ← Step 1（all_readers_ + 去重）

tests/
  memory/
    test_flush_queue.cpp     ← Step 3 验证
    test_flush_worker.cpp    ← Step 4 验证
    test_rt_index_thread.cpp ← Step 5 验证
    test_rt_flush_e2e.cpp    ← Step 6 端到端
```

---

## 11. 测试计划

| 测试文件 | 对应 Step | 验证维度 |
|----------|-----------|---------|
| `test_flush_queue.cpp` | Step 3 | push/takeBatch 语义正确；size < max_batch 时超时返回；并发 push 无丢失；shutdown() 后立即返回空 |
| `test_flush_worker.cpp` | Step 4 | 单 entry flush → SegmentReader doc_count 正确；3 entry k-way merge → doc_count = 三者之和、term df 正确；commitDiskSegment 后 frozen_reader 从 all_readers_ 消失；stop() 正常退出 |
| `test_rt_index_thread.cpp` | Step 5 | ramUsed 阈值触发 freeze；hash 利用率阈值触发 freeze；freeze 后旧 reader 仍可搜索；freeze 后新 segment 立即接受写入；shutdown() 剩余文档进入 FlushQueue |
| `test_rt_flush_e2e.cpp` | Step 6 | 全程无搜索空窗；ext_id 去重无重复结果；最终 doc_count = 写入总数；MemorySegment 无泄漏（ASAN） |
