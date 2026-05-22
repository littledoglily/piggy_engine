# 多线程索引构建设计文档

## 1. 目标与约束

### 目标
- `addDocument()` 接口对外保持兼容，内部由多线程并发消费
- 每个消费线程独立管理内存，达阈值后独立 flush 为临时 Segment
- 各线程 flush 文件无竞争（全局唯一 SegmentId）
- 所有线程结束后，K-way 合并临时 Segment → 最终 Segment，并清理临时文件
- 新增公共组件独立放到 `modules/common/`，不污染现有模块

### 约束
- C++17，标准库线程（`std::thread` + `std::mutex` + `std::condition_variable`）
- 不引入第三方依赖
- 单线程模式（N=1）输出必须与现有 `IndexWriter` 完全等价
- DocId 全局唯一，跨线程不重复

---

## 2. 整体架构

```
调用方
  │  addDocument(Document&&)          // 移动语义，零拷贝字符串
  ▼
┌─────────────────────────────────┐
│       ParallelIndexWriter       │
│  BlockingQueue<DocRef>  (有界)  │
│  ┌─────────┬─────────┬────────┐ │
│  │Worker 0 │Worker 1 │Worker N│ │   每个 Worker 独立内存
│  │SegId=0,3│SegId=1,4│       │ │   全局 atomic SegId 分配
│  └────┬────┴────┬────┴───┬───┘ │
│       │flush    │flush   │flush │
└───────┼─────────┼────────┼─────┘
        │         │        │
   _0.tim_body  _1.tim  _2.tim    临时 Segment 文件
   _0.doc_body  _1.doc  _2.doc
        │         │        │
        └────┬────┘        │
             │   K-way Merge (SegmentMerger 扩展)
             ▼
         _M.tim_body / _M.doc_body ...   最终 Segment
             │
         删除临时文件(_0~_N-1)
             │
         写 segments_G 文件
```

---

## 3. 公共组件（modules/common/）

### 3.1 BlockingQueue

```cpp
// modules/common/include/common/blocking_queue.h
template<typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(size_t capacity);
    void push(T item);                  // 满时阻塞生产者
    bool pop(T& out);                   // 空时阻塞；队列关闭返回 false
    void close();                       // 通知所有消费者退出
};
```

- 内部：`std::deque<T>` + `std::mutex` + 两个 `std::condition_variable`（not_full / not_empty）
- `close()` 设 closed 标志 + notify_all，`pop()` 在关闭且空时返回 false
- 无 sentinel 设计：close() 替代 end message，更干净

### 3.2 ThreadPool

```cpp
// modules/common/include/common/thread_pool.h
class ThreadPool {
public:
    explicit ThreadPool(size_t n_threads);
    ~ThreadPool();                          // 等待所有任务完成
    void submit(std::function<void()> fn);  // 提交任务
    void waitAll();                         // 阻塞直到所有任务结束
};
```

- 内部用 `BlockingQueue<std::function<void()>>`
- 析构时 `close()` 队列，所有线程自然退出
- 注意：ParallelIndexWriter 用固定 N 个长生命周期 worker，不用任务队列；ThreadPool 主要供通用场景

### 3.3 DocRef（零拷贝传输）

```cpp
// modules/common/include/common/doc_ref.h
// 调用方 move Document 进队列，worker 接收所有权，全程无字符串拷贝
using DocRef = std::unique_ptr<Document>;

// 辅助工厂
inline DocRef makeDocRef(Document&& doc) {
    return std::make_unique<Document>(std::move(doc));
}
```

- `unique_ptr` move 是 O(1)，`Document` 内的 `string`/`vector` 也是 move O(1)
- Queue 中存 `unique_ptr<Document>`，pop 后 worker 独占

### 3.4 FileUtils

```cpp
// modules/common/include/common/file_utils.h
namespace file_utils {
    // 列出目录下所有匹配 _<id>.si 的 segment id
    std::vector<uint32_t> listSegmentIds(const std::string& dir);
    // 删除指定 segment 的全部文件（*.tim_*, *.doc_*, *.pos_*, .fdt .fdx .liv .si .ff_* .len_*）
    void deleteSegmentFiles(const std::string& dir, uint32_t seg_id);
    // 确保目录存在
    void ensureDir(const std::string& dir);
}
```

### 3.5 KwayMerge（泛型，供测试）

K-way 合并的核心逻辑已在 `SegmentMerger::doMerge()` 中实现（posting 层面的有序归并）。  
新增 `common/kway_merge.h` 提供**泛型最小堆迭代器**，供 SegmentMerger 内部重构或单测：

```cpp
// modules/common/include/common/kway_merge.h
// 泛型 K-way 合并：接受 K 个已排序序列的迭代器，输出全局有序元素
template<typename It, typename Compare = std::less<>>
class KwayMergeIterator { ... };
```

实际 Segment 级别的合并仍由 `SegmentMerger` 完成，新增一个接口：

```cpp
// SegmentMerger 新增方法
MergeStats mergeSubset(const std::vector<uint32_t>& src_ids, uint32_t new_seg_id);
```

---

## 4. 核心类设计

### 4.1 IndexWorker（模块内部，不对外暴露）

```cpp
// modules/index/src/index_worker.h  (internal)
class IndexWorker {
public:
    IndexWorker(const std::string& dir,
                const Schema& schema,
                const std::vector<std::unique_ptr<FieldDescriptor>>& descs,
                std::atomic<uint32_t>& global_seg_id,
                std::atomic<DocId>&    global_doc_id,
                float ram_threshold_mb);

    // 消费队列，处理完所有文档后自动退出
    void run(BlockingQueue<DocRef>& queue);

    // 返回本 worker 产出的所有 segment id（供合并使用）
    const std::vector<uint32_t>& flushedSegIds() const;

private:
    void processDoc(DocRef doc);
    void maybeFlush();
    void doFlush();
    size_t estimateRam() const;

    std::string   dir_;
    const Schema& schema_;
    const std::vector<std::unique_ptr<FieldDescriptor>>& descs_;

    std::atomic<uint32_t>& g_next_seg_id_;
    std::atomic<DocId>&    g_next_doc_id_;
    float ram_threshold_mb_;

    // 独立内存状态（无锁访问，仅本 worker 使用）
    std::map<std::string, InMemoryIndex> field_indexes_;
    std::map<std::string, uint64_t>      field_token_counts_;
    std::vector<StoredDoc>               stored_docs_buf_;
    FastFieldWriter                      ff_writer_;
    size_t                               stored_ram_bytes_ = 0;
    uint32_t                             local_doc_count_  = 0;

    std::vector<uint32_t> flushed_seg_ids_;
};
```

关键点：
- `processDoc` 复用 `IndexWriter::addDocument` 的逻辑，但 DocId 来自 `g_next_doc_id_.fetch_add(1)`
- `doFlush` 调用 `SegmentWriter::flush`，SegId 来自 `g_next_seg_id_.fetch_add(1)`
- 所有字段只在本线程访问，**无锁**

### 4.2 ParallelIndexWriter

```cpp
// modules/index/include/index/parallel_index_writer.h
class ParallelIndexWriter {
public:
    ParallelIndexWriter(const std::string& dir,
                        int n_workers = 4,
                        float ram_per_worker_mb = 64.0f,
                        Schema schema = Schema::defaultSchema());
    ~ParallelIndexWriter();

    // 外部接口：移动语义，内部异步消费
    void addDocument(Document&& doc);

    // 等待所有 worker 结束，K-way 合并，写 segments 文件
    void commit();

    uint32_t totalDocs() const { return g_next_doc_id_.load() - 1; }

private:
    void startWorkers();
    void stopWorkers();
    uint32_t mergeWorkerSegments(const std::vector<uint32_t>& all_seg_ids);

    std::string   dir_;
    int           n_workers_;
    float         ram_per_worker_mb_;
    Schema        schema_;
    std::vector<std::unique_ptr<FieldDescriptor>> descs_;

    BlockingQueue<DocRef>     queue_;          // 共享队列
    std::vector<IndexWorker>  workers_;
    std::vector<std::thread>  threads_;

    std::atomic<uint32_t>     g_next_seg_id_{0};
    std::atomic<DocId>        g_next_doc_id_{1};   // DocId 1-indexed
};
```

---

## 5. 数据流与线程安全分析

| 共享资源 | 访问方 | 保护方式 |
|---------|--------|---------|
| `BlockingQueue<DocRef>` | 生产者（主线程）+ N 个 Worker | 内部 mutex |
| `g_next_seg_id_` | 所有 Worker 在 doFlush 时 | `atomic fetch_add` |
| `g_next_doc_id_` | 所有 Worker 在 processDoc 时 | `atomic fetch_add` |
| `field_indexes_` / `stored_docs_buf_` | 各 Worker 私有 | 无锁（不共享） |
| 磁盘文件 `_N.*` | 各 Worker 写各自唯一 SegId | SegId 不重复，天然无竞争 |
| `flushed_seg_ids_` | Worker 写，主线程 join 后读 | join 提供 happens-before |

---

## 6. 实现步骤与验证方法

### Step 0：搭建 common 模块骨架
**改动范围**
- 新建 `modules/common/` 目录结构
- `CMakeLists.txt` 中添加 `add_subdirectory(modules/common)`
- `inverted_index_lib` 中加入 `piggy_common`

**文件**
```
modules/common/
  CMakeLists.txt
  CLAUDE.md
  include/common/
    blocking_queue.h
    thread_pool.h
    doc_ref.h
    file_utils.h
    kway_merge.h
  src/
    file_utils.cpp
    thread_pool.cpp
```

**验证**
```bash
# 编写 tests/common/test_blocking_queue.cpp
./build/test_blocking_queue
# 测试内容：
# 1. 单生产者单消费者，1000 条消息，顺序正确
# 2. 多生产者多消费者，无消息丢失，无死锁
# 3. close() 后消费者正常退出
# 4. 满队列时生产者阻塞，消费者消费后解除阻塞

./build/test_thread_pool
# 测试内容：
# 1. 提交 N 个任务，全部完成后 waitAll 返回
# 2. 析构时 pending 任务全部完成
```

---

### Step 1：全局 DocId / SegId 分配 + DocRef
**改动范围**
- `modules/common/include/common/doc_ref.h` 实现
- `ParallelIndexWriter` 骨架（只含 atomic 计数器，暂无线程）

**验证**
```bash
# 单元测试：并发 1000 个线程各 fetch_add 1000 次
# 结果集合大小 == 1000*1000，无重复
# 用 std::set 去重后 size 不变
```

---

### Step 2：IndexWorker 实现（单 Worker 等价性）
**改动范围**
- `modules/index/src/index_worker.h` + `index_worker.cpp`
- 复用 `IndexWriter::addDocument` 中的字段处理逻辑（提取为 `processFields` 静态函数）

**验证**
```bash
# 构造 ParallelIndexWriter(n_workers=1, ram=16MB)
# 用 demo 数据索引 100 篇文档
# 对比输出：
#   单线程 IndexWriter 产出 _0.tim_body
#   1-Worker 产出 _0.tim_body（merge 后）
# diff 两个文件或比较 term_count/total_docs/每个 term 的 df

./build/test_parallel_worker_equivalence
```

---

### Step 3：多 Worker 并发 + 文件无竞争验证
**改动范围**
- `ParallelIndexWriter` 启动 N 个线程
- `BlockingQueue` 接入
- 各 Worker 独立 flush，SegId 全局递增

**验证**
```bash
# N=4，索引 demo 数据
# 检查无文件名冲突（ls -la build/test_parallel_index/ | grep _[0-9]*.si）
# 所有 SegId 唯一
# 各 Worker 内存统计正确（每个 flush 打印 worker_id + seg_id + doc_count）
./build/test_parallel_no_conflict
```

---

### Step 4：Sentinel / close() 机制 + Worker 正常退出
**改动范围**
- `ParallelIndexWriter::commit()` 调用 `queue_.close()`
- Worker `run()` 在 `pop()` 返回 false 时触发最终 `doFlush()`

**验证**
```bash
# 索引 N 篇文档后立即 commit()
# 检查 sum(worker[i].doc_count) == N
# 检查所有 .si 文件存在且可被 SegmentReader 加载
./build/test_parallel_commit
```

---

### Step 5：SegmentMerger 扩展（mergeSubset）
**改动范围**
- `SegmentMerger` 新增 `mergeSubset(vector<uint32_t> src_ids, uint32_t new_seg_id)`
- 内部：加载指定 id 的 SegmentReader，调用已有 `doMerge`

**验证**
```bash
# 构造 2 个已知 Segment（固定数据），调用 mergeSubset
# 验证输出 Segment 的 term_count == sum(input term_counts) - duplicates
# 验证 doc_count == sum(input doc_counts)
# 验证 posting list 有序无重复 DocId
./build/test_segment_merge_subset
```

---

### Step 6：commit() 完整流程 + 临时文件清理
**改动范围**
- `ParallelIndexWriter::commit()`:
  1. `queue_.close()`，join 所有 worker 线程
  2. 收集 all_seg_ids（所有 worker 的 flushed_seg_ids_）
  3. 调用 `mergeSubset(all_seg_ids, final_seg_id)` → 最终 Segment
  4. 调用 `file_utils::deleteSegmentFiles()` 删临时文件
  5. 写 `segments_G` 文件

**验证**
```bash
# 完整 E2E：用 4 线程索引 1000 篇文档
# commit() 后目录中只剩最终 Segment（_M.*）
# 用 wiki_searcher --index 搜索，结果与单线程建索引相同
./build/test_parallel_e2e
# 还可以跑 diff：
#   单线程输出 posting list → a.txt
#   4线程输出 posting list  → b.txt
#   diff <(sort a.txt) <(sort b.txt)  # term+df 完全一致
```

---

## 7. 文件结构

```
modules/
  common/                        ← 新建
    CMakeLists.txt
    CLAUDE.md
    include/common/
      blocking_queue.h           ← Step 0
      thread_pool.h              ← Step 0
      doc_ref.h                  ← Step 1
      file_utils.h               ← Step 0
      kway_merge.h               ← Step 5（可选，泛型辅助）
    src/
      file_utils.cpp
      thread_pool.cpp

  index/
    include/index/
      parallel_index_writer.h    ← Step 2~6（新文件，不改 index_writer.h）
    src/
      index_worker.h             ← Step 2（仅模块内可见）
      index_worker.cpp           ← Step 2~4
      parallel_index_writer.cpp  ← Step 2~6
      segment_merger.cpp         ← Step 5（新增 mergeSubset 方法）
      segment_merger.h           ← Step 5（新增声明）

tests/
  common/
    test_blocking_queue.cpp      ← Step 0
    test_thread_pool.cpp         ← Step 0
  parallel/
    test_parallel_worker_equivalence.cpp  ← Step 2
    test_parallel_no_conflict.cpp         ← Step 3
    test_parallel_commit.cpp              ← Step 4
    test_segment_merge_subset.cpp         ← Step 5
    test_parallel_e2e.cpp                 ← Step 6

CMakeLists.txt                   ← 添加 common 模块 + 新测试目标
```

---

## 8. 关键设计决策

### Q1：为什么用 `queue_.close()` 而不是 N 个 sentinel？
sentinel 需要知道 worker 数量且在队列满时可能堵塞。`close()` 是广播，无此问题。

### Q2：DocId 为什么不在 IndexWriter 层分配，而是 Worker 层原子获取？
Worker 层分配避免主线程成为瓶颈，也保证在 Document 进入内存索引时 DocId 已确定（SegmentWriter 写文件需要 DocId）。

### Q3：合并策略：merge all vs tiered merge？
本次设计先实现 merge all（所有 worker 临时 Segment → 一个最终 Segment）。  
对于超大数据集（> RAM），可后续改为 tiered：每 K 个临时 Segment 合并一次，减少中间内存峰值。

### Q4：avgdl 跨线程如何准确计算？
每个 Worker 只有本地 token_count / doc_count，flush 时 avgdl 是局部近似。  
合并阶段可用合并后的全局 avgdl 重算 UB（已有 SegmentMerger 的 IDF/UB 重算逻辑）。  
这与现有单线程行为一致（flush 时也是近似，merge 时重算）。

### Q5：FastFieldWriter 如何保证对齐？
FastField 文件是数组格式，doc index 必须连续。Worker 各自写本地 FastField，merge 阶段按 DocId 顺序重写——**这需要 SegmentMerger 在合并 posting list 时同时合并 FastField**。当前 merger 已有此逻辑，新接口直接复用。

---

## 9. 风险与后续

| 风险 | 缓解措施 |
|-----|---------|
| Worker 崩溃导致临时文件泄漏 | commit() 失败时，可在重启时扫描无对应 segments_N 条目的孤立 .si 文件并清理 |
| 内存峰值 = N * ram_threshold | 文档建议 ram_per_worker = total_ram / (N+1)，留 1 份给合并 |
| K-way merge 时 Segment 数量过多 | 限制 max_segments_before_merge，超过时在 commit 前做一轮 intermediate merge |
| 写索引与搜索并发 | ParallelIndexWriter 不解决在线读写并发，搜索仍需在 commit() 完成后重新打开 IndexSearcher |

---

## 10. 已知问题（待 parallel_index_build 所有 Step 完成后优化）

### 10.1 SegmentMerger::doMerge 内存峰值过高

**问题**：按 1500 万文档估算，`doMerge` 总内存峰值约 **10–17 GB**，主因如下：

| 来源 | 估算（1500万文档，body 平均 300B） | 说明 |
|---|---|---|
| SegmentReader × N（fdx + len + tim + liv）| ~380 MB | 随 input segment 数线性增长 |
| `alive_docs.str_fields`（所有文档原文同时驻留）| **~8.5 GB** | **最大瓶颈** |
| `remap`（`unordered_map<uint64_t, DocId>`）| ~760 MB | 链式哈希节点开销大 |
| `merged_doc_lens`（`std::map<DocId, uint32_t>`，每字段重建）| ~720 MB | 红黑树节点 ~48B/条 |
| `merged_pos` 瞬时峰（高频词）| ~10–50 MB | 按 term 释放，可接受 |

**根本原因**：`alive_docs` 同时承担"remap 辅助"和"原文存储"两个职责，字段原文从 Step1 一直压到 Step5 写完 `.fdt` 才释放。

**已完成优化（2025-05-22）**：

1. ✅ **`GlobalDoc` 去掉 `str_fields` / `ext_id`**：新增 `local_pos`（segment 内 1-indexed 位置）和 `new_doc_id`（预计算避免 remap 二次查询）。Step 5 写 `.fdt` 时惰性调用 `readStoredDoc(local_pos)`，单条读完即释放，峰值降至 O(1) per doc。节省 ~8 GB（300B avg × 1500万文档）。

2. ✅ **`merged_doc_lens` 改为 `vector<uint32_t>`**：`output_doc_count` 在 Step1 完成后已知，直接预分配；下标 = new_doc_id（1-indexed），从 `std::map` 的 ~48B/条降到 4B/条。节省 ~660 MB。

3. ✅ **FastField 索引改用 `local_pos - 1`**：修正并行模式下 FastField 读取以 global doc_id 作为 0-indexed 数组下标的潜在越界问题。

**待考虑优化**：

- **`remap` 改为排序数组 + 二分查找**（可选）：`vector<pair<uint64_t, DocId>>` 排序后二分，节省链式哈希节点开销，约节省 ~400 MB；代价是构造时需要排序 O(N log N)。

优化后预估峰值：**~1.2 GB**（主要剩 remap ~760MB + SegmentReader fdx/len ~380MB）。
