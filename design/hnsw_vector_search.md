# HNSW 向量检索集成设计

## 背景与目标

piggy_engine 目前只支持基于倒排索引的全文检索（BM25）。引入 HNSW（Hierarchical Navigable Small World）后，支持：

- **纯向量检索**：KNN 语义搜索（embedding 相似度）
- **混合检索**：BM25 分 + 向量分 → RRF 融合排序

设计原则与现有系统保持一致：
- 新增 `vector` 独立模块，不污染现有模块
- 向量字段通过 Schema 声明，与现有字段类型平行
- IndexSearcher 统一入口，对外接口改动最小
- 实时写入（RT）同样支持向量字段

---

## 一、架构总览

```
modules/
  vector/           ← 新模块
    include/vector/
      hnsw_index.h        HNSW 图核心（构建 + 搜索）
      hnsw_writer.h       图序列化（flush 到 .hnsw_<field>）
      hnsw_reader.h       图反序列化 + 查询（实现 IVectorReader）
      i_vector_reader.h   向量读接口（独立于 ISegmentReader）
      memory_hnsw.h       内存 HNSW（RT 路径）
    src/
      hnsw_index.cpp
      hnsw_writer.cpp
      hnsw_reader.cpp
      memory_hnsw.cpp
```

### 模块依赖

```
core ◄── vector   （vector 只依赖 core/types.h，无其他依赖）
          ▲
          └── index  （HnswWriter/Reader 写磁盘，复用 index 文件路径约定）
          ▲
          └── query  （IndexSearcher 持有 IVectorReader 列表，调用 knnSearch）
          ▲
          └── memory （MemoryHnsw 在 RT flush 时由 HnswWriter 序列化）
```

---

## 二、Schema 扩展

### 2.1 新增 FieldType::Vector

```cpp
// field/schema.h
enum class FieldType {
    Text, Keyword, Int64, Float32, Combined,
    Vector    // ← 新增
};

struct FieldSchema {
    std::string name;
    FieldType   type;
    IndexOption index   = IndexOption::None;
    bool        stored  = false;
    bool        fast    = false;
    bool        multi   = false;
    std::vector<std::string> sources;

    // Vector 字段专属（type == Vector 时有效）
    uint32_t    vec_dim    = 0;       // 向量维度
    std::string vec_metric = "cosine"; // "cosine" | "l2" | "dot"
    uint32_t    hnsw_m     = 16;      // HNSW M 参数
    uint32_t    hnsw_ef_construction = 200;
};
```

JSON Schema 示例：
```json
{
  "fields": [
    { "name": "title",     "type": "text",    "index": "freqs_positions", "stored": true },
    { "name": "embedding", "type": "vector",  "vec_dim": 768, "vec_metric": "cosine",
      "hnsw_m": 16, "hnsw_ef_construction": 200 }
  ]
}
```

### 2.2 FieldVal 扩展

```cpp
// core/types.h
using FieldVal = std::variant<
    std::string,
    int64_t,
    float,
    std::vector<std::string>,
    std::vector<float>   // ← 新增，Vector 字段使用
>;
```

Document 写入示例：
```cpp
doc.set("embedding", std::vector<float>{0.1f, 0.2f, ..., 0.768f});
```

---

## 三、向量读接口（IVectorReader）

向量检索与倒排检索语义不同（近似 KNN vs 精确倒排），用独立接口而不是继承 ISegmentReader，避免污染现有接口：

```cpp
// vector/i_vector_reader.h
namespace ii {

struct VectorHit {
    DocId    doc_id;
    uint64_t ext_id;
    float    score;    // 相似度分（cosine: [0,1]；L2: 取负距离）
};

class IVectorReader {
public:
    virtual ~IVectorReader() = default;

    // KNN 检索：返回 top_k 个最相近的 doc
    virtual std::vector<VectorHit> knnSearch(
        const std::string&         field,
        const std::vector<float>&  query_vec,
        uint32_t                   top_k,
        uint32_t                   ef_search = 50
    ) const = 0;

    // 该 reader 是否包含指定向量字段
    virtual bool hasVectorField(const std::string& field) const = 0;

    // 当前 reader 包含的文档数（用于跨 segment 归并时加权）
    virtual uint32_t docCount() const = 0;

    // 关联的 segment id（与 ISegmentReader::segmentId() 对应）
    virtual uint32_t segmentId() const = 0;

    // ext_id 查询（用于 KNN 结果与 BM25 结果做 RRF 融合时的去重）
    virtual uint64_t getExtId(DocId doc_id) const = 0;
};

} // namespace ii
```

---

## 四、HNSW 核心实现

### 4.1 数据结构

```cpp
// vector/hnsw_index.h
namespace ii::vector {

enum class Metric { Cosine, L2, Dot };

struct HnswConfig {
    uint32_t dim;
    uint32_t M              = 16;
    uint32_t ef_construction = 200;
    Metric   metric         = Metric::Cosine;
};

class HnswIndex {
public:
    explicit HnswIndex(HnswConfig cfg);

    // 构建时插入（非线程安全，单写）
    void insert(DocId doc_id, const float* vec);

    // 查询（线程安全，多读）
    std::vector<std::pair<DocId, float>> search(
        const float* query_vec,
        uint32_t top_k,
        uint32_t ef_search = 50
    ) const;

    uint32_t size() const;
    const HnswConfig& config() const;

    // 序列化接口（供 HnswWriter 调用）
    void forEachNode(std::function<void(DocId, const float*, const std::vector<std::vector<DocId>>&)>) const;

private:
    HnswConfig cfg_;

    struct Node {
        DocId                           doc_id;
        std::vector<float>              vec;     // 原始向量
        std::vector<std::vector<DocId>> links;   // links[layer] = 邻居列表
    };

    std::vector<Node>             nodes_;         // doc_id → Node
    std::unordered_map<DocId, uint32_t> id_to_idx_; // doc_id → nodes_ 下标

    uint32_t entry_point_ = UINT32_MAX;
    uint32_t max_layer_   = 0;
    mutable std::shared_mutex rw_mutex_;  // 读写锁（RT 并发读）

    // 内部算法
    float    distance(const float* a, const float* b) const;
    uint32_t randomLevel() const;
    std::vector<DocId> searchLayer(const float* q, DocId ep, uint32_t ef, uint32_t layer) const;
    void     connect(uint32_t node_idx, const std::vector<DocId>& neighbors, uint32_t layer);
};

} // namespace ii::vector
```

### 4.2 距离函数

| Metric | 公式 | 返回值（越大越好） |
|--------|------|------------------|
| Cosine | `dot(a,b) / (|a| * |b|)` | [-1, 1]，通常 embedding 归一化后 = dot product |
| L2     | `-sqrt(sum((a_i-b_i)^2))` | 取负，越接近 0 越相似 |
| Dot    | `sum(a_i * b_i)` | 未归一化内积 |

实现时优先考虑 SIMD 优化（见 Step 7）。

---

## 五、磁盘格式（.hnsw_<field>）

```
_N.hnsw_<field>:
┌─────────────────────────────────────────┐
│ Header (固定 32B)                        │
│   magic:    4B  = 0x484E5357 ('HNSW')   │
│   version:  2B  = 1                     │
│   metric:   1B  (0=cosine,1=l2,2=dot)  │
│   reserved: 1B                          │
│   dim:      4B                          │
│   M:        4B                          │
│   ef_con:   4B                          │
│   node_count: 4B                        │
│   entry_point_doc_id: 4B               │
│   max_layer: 4B                        │
├─────────────────────────────────────────┤
│ Node 区（node_count 个节点，顺序存储）   │
│   per node:                             │
│     doc_id:     4B                      │
│     ext_id:     8B                      │
│     layer_cnt:  1B  (该节点最高层数+1)  │
│     vec:        dim*4B                  │
│     per layer (从0到layer_cnt-1):       │
│       neighbor_count: 2B               │
│       neighbor_doc_ids: count*4B       │
└─────────────────────────────────────────┘
```

节点按插入顺序存储，加载时重建 `id_to_idx_` 哈希表。整个图加载进内存（与 `.tim` 词典策略一致）。

---

## 六、IndexSearcher 集成

### 6.1 新增向量 Reader 管理

```cpp
// query/index_searcher.h（新增部分）
#include "vector/i_vector_reader.h"

class IndexSearcher {
public:
    // 向量 reader 管理（与 addReader 并行维护）
    void addVectorReader   (std::shared_ptr<IVectorReader> reader);
    void removeVectorReader(std::shared_ptr<IVectorReader> reader);
    void commitVectorSegment(std::shared_ptr<IVectorReader>              new_disk,
                             std::vector<std::shared_ptr<IVectorReader>> frozen_to_remove);

    // ── 纯 KNN 检索 ─────────────────────────────────────────────────────────
    std::vector<SearchResult> searchKnn(
        const std::string&         field,
        const std::vector<float>&  query_vec,
        uint32_t                   top_k    = 10,
        uint32_t                   ef_search = 50
    ) const;

    // ── 混合检索（BM25 + KNN → RRF 融合） ───────────────────────────────────
    std::vector<SearchResult> searchHybrid(
        const std::string&         text_query,  // BM25 查询
        const std::string&         vec_field,   // 向量字段名
        const std::vector<float>&  query_vec,   // 查询向量
        uint32_t                   top_k    = 10,
        float                      rrf_k    = 60.0f,  // RRF 常数
        uint32_t                   ef_search = 50,
        QueryMode                  mode     = QueryMode::OR
    ) const;

private:
    mutable std::shared_mutex                     vec_readers_mutex_;
    std::vector<std::shared_ptr<IVectorReader>>   vec_readers_;

    // RRF 融合实现（见 §6.2）
    std::vector<SearchResult> rrfFusion(
        const std::vector<SearchResult>& bm25_results,
        const std::vector<VectorHit>&    knn_results,
        uint32_t top_k,
        float    rrf_k
    ) const;
};
```

### 6.2 RRF 融合算法

Reciprocal Rank Fusion 不依赖分数归一化，适合异构评分系统：

```
rrf_score(doc) = Σ_system  1 / (k + rank(doc, system))
```

其中 k=60（经验值，减少高排名文档的支配效应）。

```
BM25 结果：[d1(0.9), d3(0.7), d5(0.5)]   → ranks: d1=1, d3=2, d5=3
KNN  结果：[d3(0.95), d2(0.88), d1(0.7)] → ranks: d3=1, d2=2, d1=3

rrf(d1) = 1/(60+1) + 1/(60+3) = 0.01639 + 0.01563 = 0.03202
rrf(d3) = 1/(60+2) + 1/(60+1) = 0.01613 + 0.01639 = 0.03252   ← 最高
rrf(d2) = 0        + 1/(60+2) = 0.01613
rrf(d5) = 1/(60+3) + 0        = 0.01563

最终排序：d3 > d1 > d2 > d5
```

---

## 七、实时索引支持（RT 路径）

### 7.1 MemoryHnsw（内存阶段）

```cpp
// vector/memory_hnsw.h
namespace ii::vector {

class MemoryHnsw : public IVectorReader {
public:
    explicit MemoryHnsw(HnswConfig cfg, uint32_t seg_id);

    // 写侧：RT 写入线程调用
    void insert(DocId doc_id, uint64_t ext_id, const float* vec);

    // IVectorReader 接口
    std::vector<VectorHit> knnSearch(const std::string& field,
                                     const std::vector<float>& q,
                                     uint32_t top_k, uint32_t ef) const override;
    bool     hasVectorField(const std::string& field) const override;
    uint32_t docCount()  const override;
    uint32_t segmentId() const override { return seg_id_; }
    uint64_t getExtId(DocId doc_id)    const override;

    // 序列化（FlushWorker 调用）
    void flush(const std::string& dir, uint32_t seg_id, const std::string& field) const;

private:
    HnswConfig                cfg_;
    std::string               field_name_;
    uint32_t                  seg_id_;
    HnswIndex                 index_;
    std::unordered_map<DocId, uint64_t> ext_ids_;
    mutable std::shared_mutex rw_mutex_;
};

} // namespace ii::vector
```

### 7.2 FlushWorker 扩展

FlushWorker flush 磁盘 Segment 时，同步序列化对应的向量图：

```
FlushWorker::doFlush() 现有流程：
  1. MemorySegment::flushToDisk()  → _N.{tim,doc,pos,len,fdt,fdx,liv,si}
  2. [新增] MemoryHnsw::flush()    → _N.hnsw_<field>
  3. HnswReader::open(dir, N)      → 加载磁盘图
  4. searcher.commitVectorSegment(new_hnsw_reader, {frozen_memory_hnsw})
```

---

## 八、落地 Steps

### Step 1：Schema 扩展（1-2 天）

**改动文件**：
- `modules/field/include/field/schema.h` — 添加 `FieldType::Vector`，`FieldSchema` 增加 `vec_dim / vec_metric / hnsw_m / hnsw_ef_construction`
- `modules/field/src/schema.cpp` — JSON 加载/保存处理 Vector 字段
- `modules/core/include/core/types.h` — `FieldVal` 增加 `std::vector<float>` variant，`Document::set(name, vector<float>)`

**验证**：
```bash
# 单元测试：Vector 字段序列化/反序列化
./build/test_schema   # 扩展现有 test_schema，增加 Vector 字段的 toJson/fromJson roundtrip
```

---

### Step 2：HnswIndex 核心（3-5 天）

**新建文件**：
- `modules/vector/src/hnsw_index.cpp` — HNSW 图构建 + 搜索

实现要点：
- `insert()` 分层插入，`randomLevel()` 按指数分布采样
- `searchLayer()` 贪心 beam search（ef 大小候选集）
- 邻居选择用 heuristic（保留连通多样性，而不是纯距离最近）
- `search()` 加 `shared_lock`，`insert()` 加 `unique_lock`

**验证**：
```bash
# 新增 tests/vector/test_hnsw_index.cpp
./build/test_hnsw_index
# 测试 case：
# - insert 1000 个随机 128-dim 向量，search recall@10 > 0.95
# - cosine / l2 / dot 三种 metric 正确性
# - 并发读（10 线程同时 search）不 crash
```

---

### Step 3：磁盘序列化（1-2 天）

**新建文件**：
- `modules/vector/src/hnsw_writer.cpp` — `HnswIndex → .hnsw_<field>` 文件
- `modules/vector/src/hnsw_reader.cpp` — 加载磁盘图，实现 `IVectorReader`

**验证**：
```bash
# 新增 tests/vector/test_hnsw_io.cpp
./build/test_hnsw_io
# - write → read roundtrip，search 结果与内存版一致
# - 空图、单节点、10K 节点各边界 case
```

---

### Step 4：IndexSearcher 集成（2-3 天）

**改动文件**：
- `modules/query/include/query/index_searcher.h` — 增加 `vec_readers_`、`addVectorReader`、`searchKnn`、`searchHybrid`
- `modules/query/src/index_searcher.cpp` — 实现以上接口，RRF 融合逻辑

**验证**：
```bash
# 新增 tests/query/test_vector_searcher.cpp
./build/test_vector_searcher
# - 纯 KNN：插入 100 doc，向量检索返回正确 top-k
# - RRF 融合：BM25 结果 + KNN 结果，验证 RRF 分计算
# - ext_id 去重：跨 segment 同 ext_id 只保留最高 rrf 分
```

---

### Step 5：实时索引支持（2-3 天）

**新建文件**：
- `modules/vector/src/memory_hnsw.cpp` — 实现 `MemoryHnsw`

**改动文件**：
- `modules/memory/src/flush_worker.cpp` — flush 时调用 `MemoryHnsw::flush()`
- `modules/memory/include/memory/rt_index_thread.h` — RT 线程持有 `MemoryHnsw` 实例，`addDocument` 时同步写入

**验证**：
```bash
# 新增 tests/memory/test_rt_vector.cpp
./build/test_rt_vector
# - RT 写入 → flush → 磁盘 KNN 可检索
# - epoch 切换期间搜索不丢结果
# - flush 后 MemoryHnsw 释放，HnswReader 接管
```

---

### Step 6：wiki_indexer / wiki_searcher 集成（1-2 天）

**改动文件**：
- `tools/wiki_indexer.cpp` — 增加 `--embedding-field` 参数，读取预计算向量（JSON 中的 float 数组字段）
- `tools/wiki_searcher.cpp` — 增加 `--vec-query <float列表>` 和 `--hybrid` 参数

**验证**：
```bash
./build/wiki_indexer --input ./data --output ./wiki_index \
    --schema schema_with_vec.json --ram 256

# 纯 KNN
./build/wiki_searcher --index ./wiki_index \
    --vec-query "0.1,0.2,...,0.768" --vec-field embedding --top 5

# 混合检索
./build/wiki_searcher --index ./wiki_index \
    --query "body:python" --vec-query "..." --vec-field embedding --hybrid --top 5
```

---

### Step 7（可选）：SIMD 加速距离计算

PForDelta 的 SIMD 优化与此同步进行。向量距离是检索热路径：

- **AVX2**：一次处理 8 个 float（256-bit 寄存器），Cosine / L2 内积展开为 `_mm256_dp_ps` / `_mm256_fmadd_ps`
- **Apple Silicon**：ARM NEON，`vfmaq_f32`
- 编译时检测：`#ifdef __AVX2__` / `#ifdef __ARM_NEON`

---

## 九、关键设计决策

### 为什么用独立 IVectorReader 而不是扩展 ISegmentReader？

ISegmentReader 的接口语义是"单个 doc 的 term 匹配 + 字段长度"，KNN 是"batch 返回近似邻居列表"，两者迭代模型完全不同。混在同一接口会：

1. 迫使所有 ISegmentReader 实现（包括 MockSegmentReader）都要空实现 KNN 接口
2. 掩盖向量检索与标量检索生命周期不同（向量图整体加载 vs 倒排表惰性解压）

独立接口后，IndexSearcher 维护两套并行 reader 列表，分别加锁，逻辑清晰。

### 为什么用 RRF 而不是线性组合？

线性组合 `α * bm25 + β * cosine` 需要先归一化两个分数到同一量纲（BM25 范围不固定，cosine 是 [-1,1]），归一化本身依赖全局 max/min，在增量写入场景下不稳定。

RRF 只使用排名而不使用分数，对两端分布无假设，在工业实践中（Elasticsearch、Vespa）是混合检索的首选策略。

### HNSW 内存占用

```
每节点内存 ≈ dim * 4B（向量）+ M * 2 * 4B（每层邻居，平均 2 层）
100万文档，dim=768，M=16：
  向量：768 * 4 * 1M ≈ 3 GB
  图：  16 * 2 * 4 * 1M ≈ 128 MB
  总计：~3.1 GB
```

大规模场景可引入量化（PQ / SQ8）压缩向量，此为后续优化项。

---

## 十、验证 Checklist

| 验证项 | 测试二进制 | 通过标准 |
|--------|-----------|---------|
| Schema Vector 字段 JSON roundtrip | `test_schema` | 无 diff |
| HNSW 构建正确性 | `test_hnsw_index` | recall@10 > 0.95（10K 随机向量）|
| HNSW 并发读安全 | `test_hnsw_index` | 10 线程 × 1000 query，无 crash / TSAN 报警 |
| 磁盘序列化 roundtrip | `test_hnsw_io` | search 结果与内存版完全一致 |
| 纯 KNN 检索 | `test_vector_searcher` | top-k 与暴力搜索结果重叠 ≥ 90% |
| RRF 融合计算 | `test_vector_searcher` | RRF 分手算验证 |
| 跨 segment 去重 | `test_vector_searcher` | 同 ext_id 只保留一条结果 |
| RT 写入 + flush + KNN 可见 | `test_rt_vector` | flush 后可检索，切换期间无丢失 |
| 现有测试无回归 | 全部 38 个现有测试 | 全部通过 |

---

## 十一、文件改动汇总

| 文件 | 类型 | 改动 |
|------|------|------|
| `modules/core/include/core/types.h` | 改 | `FieldVal` 增加 `vector<float>`，`Document::set` 重载 |
| `modules/field/include/field/schema.h` | 改 | `FieldType::Vector`，`FieldSchema` 增 vec_* 字段 |
| `modules/field/src/schema.cpp` | 改 | JSON 序列化/反序列化 Vector 字段 |
| `modules/query/include/query/index_searcher.h` | 改 | 向量 reader 管理 + `searchKnn` + `searchHybrid` |
| `modules/query/src/index_searcher.cpp` | 改 | 实现以上接口 |
| `modules/memory/src/flush_worker.cpp` | 改 | flush 时调用 `MemoryHnsw::flush()` |
| `modules/memory/include/memory/rt_index_thread.h` | 改 | 持有 `MemoryHnsw`，`addDocument` 时写入 |
| `modules/vector/` | 新建 | 整个 vector 模块（6 个源文件 + 5 个头文件） |
| `tests/vector/` | 新建 | `test_hnsw_index` / `test_hnsw_io` / `test_vector_searcher` / `test_rt_vector` |
| `tools/wiki_indexer.cpp` | 改 | `--embedding-field` 参数 |
| `tools/wiki_searcher.cpp` | 改 | `--vec-query` / `--hybrid` 参数 |
| `CMakeLists.txt` | 改 | 注册 `piggy_vector` STATIC 库，注册 4 个新测试 |
