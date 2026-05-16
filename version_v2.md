# Version v2 实现计划

> 基准日期：2026-05-16
> 前置版本：v1（倒排索引核心链路完整，text/term 级检索可用）

---

## 一、v1 已实现功能清单

| 模块 | 功能 | 文件 |
|------|------|------|
| tokenizer | CharFilter → Tokenizer → StopFilter → Porter Stemmer | `tokenizer/analyzer` |
| postings | PForDelta 压缩 / 解压，Block 大小 128 | `postings/pfor_delta` |
| postings | Block 级两层 SkipList，含 max_score / byte_offset | `postings/skiplist` |
| postings | 内存倒排链，同 doc 累加 tf + positions | `postings/posting_list` |
| segment | flush → `.tim/.doc/.pos/.fdt/.fdx/.liv/.si` | `segment/segment_writer` |
| segment | `.tim` 全量加载，`.doc/.pos/.fdt` 按需 seek | `segment/segment_reader` |
| segment | 多 Segment 合并，软删除 + doc_id 重编号 | `segment/segment_merger` |
| core | RAM Buffer + 自动 flush，commit 持久化 | `core/index_writer` |
| query | AND（Zigzag 双指针）+ OR（WAND 上界剪枝）TopK | `query/index_searcher` |
| query | 跨 Segment 归并，BM25 近似打分 | `query/index_searcher` |

---

## 二、v1 已知缺口（延续至 v2 修复）

### 缺口 1：Posting List 全量解压，内存 O(N)

```
现状：readPostingList() → 一次解压所有 Block 到 vector
目标：惰性迭代器（参考 Tantivy BlockSegmentPostings）
      SegmentReader::postingIterator(term) → Iterator
      Iterator::advance(target) → 按需解压 1 个 Block，O(1) 内存
```

**影响**：AND 多 term 交集时，大 term 的 posting 全量在内存。改为迭代器后，内存从 O(N) → O(1)，且 advance() 可直接利用 SkipList 跳到目标 Block。

### 缺口 2：BlockMaxWAND 剪枝未消费

```
现状：TermMeta.upper_bound 和 SkipNode.max_score 已写入磁盘，但 searchOR_WAND() 未用于 Block 级跳过
目标：实现 BlockMaxWAND
      维护 pivot term，当 pivot 前所有 term upper_bound 之和 < heap 最小分
      → 整个 Block 跳过，不解压，不打分
      理论：可跳过 90%+ 无效 Block
```

### 缺口 3：位置信息写入但未用于短语检索

```
现状：.pos 文件完整写入，readPosEntries() 可读取，但 searchAND/searchOR 不支持短语查询
目标：PhraseQuery — 给定 ["python", "tutorial"]，要求二者位置相差为 1
```

---

## 三、v2 新增功能：FastField 列存（Phase 1）

### 3.1 设计原则

- 一个数值字段对应一个列文件（`_N.ff_<fieldname>`）
- 定长存储（`int64_t` 8B 或 `float` 4B），下标即 local_doc_id，O(1) 随机访问
- Phase 1 不压缩，保持 seek 简单性；Phase 2（BKD）阶段再引入块压缩
- 与 Segment 生命周期绑定：flush 时写入，merge 时重建，软删除无需修改（靠 .liv 过滤）

### 3.2 支持的字段类型

```
当前 Document 中的数值字段（来自 tools/file_*.json 数据格式）：
  pubtime   int64    Unix 时间戳，用于日期范围过滤 + 排序
  uid       int64    用户 ID，用于等值过滤
  page_rank float    页面权重，用于打分 boost
```

### 3.3 文件格式

```
_N.ff_pubtime：
  [int64_t][int64_t][int64_t]...   每个 local_doc_id 对应 8 字节
  offset = local_doc_id × 8        O(1) 随机访问

_N.ff_uid：同上，int64_t

_N.ff_pagerank：每条 4 字节 float
```

### 3.4 写入流程（IndexWriter → SegmentWriter）

```
addDocument(doc):
  1. 原有：analyzer → posting list（不变）
  2. 新增：提取 doc.pubtime / doc.uid / doc.page_rank
           暂存到 FastFieldBuffer（与 stored_docs_buf_ 同步）

flush():
  1. 原有：writeTim / writeDoc / writePos / writeFdt / writeFdx / writeLiv / writeSi
  2. 新增：FastFieldWriter::flush()
           按 local_doc_id 顺序写 _N.ff_pubtime / _N.ff_uid / _N.ff_pagerank
```

### 3.5 读取接口（SegmentReader 扩展）

```cpp
// 按 doc_id O(1) 读取单个字段值
int64_t  readInt64Field(const std::string& field, DocId local_doc_id) const;
float    readFloatField(const std::string& field, DocId local_doc_id) const;

// 范围扫描（Phase 1 全列 O(N) 扫描，Phase 2 改为 BKD）
std::vector<DocId> rangeFilter(
    const std::string& field,
    int64_t lo, int64_t hi   // 闭区间
) const;

// 读取整列（排序 / 聚合用）
std::vector<int64_t> readAllInt64(const std::string& field) const;
```

### 3.6 在查询管道中的使用场景

| 场景 | 角色 | 实现方式 |
|------|------|---------|
| `pubtime:[T1,T2]` 范围过滤 | **In-filter** | 遍历 posting list 时，逐 doc 调用 `readInt64Field()` 检查，O(1)/doc |
| `pubtime:[T1,T2] AND text:"python"` | **Pre-filter**（Phase 2 后） | BKD 先给出 DocIdSet，再与 posting list 做 AND |
| 按 `pubtime` 排序结果 | **Post-sort** | 取 top-K 后，批量 `readAllInt64()` 重排序 |
| `page_rank` 参与 BM25 boost | **Score 参与** | `score = bm25 × (1 + α × page_rank_norm)` |
| `category = "Environment"` | 无需 FastField | 已有倒排索引处理等值 string 字段 |
| `uid = 890` 等值过滤 | 无需 FastField | 同上，uid 作为 term 写入倒排 |

---

## 四、v2 新增功能：BKD Tree 数值范围索引（Phase 2）

### 4.1 为什么不用 B+ tree

| 维度 | B+ tree | BKD tree |
|------|---------|---------|
| 构建方式 | 逐条插入，在线 rebalance | 批量排序后自底向上构建，一次性写入 |
| 磁盘布局 | 节点指针散布，随机 I/O | 叶块连续存储，顺序 I/O，cache 友好 |
| 多维支持 | 原生 1D | 原生 k 维（多字段联合范围） |
| Segment merge | 合并时重建代价高 | 重排序 + 重建，契合 merge 流程 |
| 更新 | 支持原地更新（搜索引擎用不到） | 不可变，配合 Segment 不可变架构 |

搜索引擎使用不可变 Segment，B+ tree 的增量更新优势完全用不上。BKD 专为批量写入 + 只读查询设计，Lucene 6.x 起用 BKD 替换了所有数值 trie 索引。

### 4.2 BKD Tree 结构

```
构建输入：(value: int64, local_doc_id: uint32) 对的有序数组
构建过程：
  1. 按 value 排序
  2. 每 1024 个点为一个叶块（leaf block）
  3. 内部节点存储每个子树的 [min_value, max_value] 边界
  4. 自底向上构建完整树

文件：_N.bkd_<fieldname>
  ├── 内部节点区（固定格式，全量加载到内存）
  └── 叶块区（顺序存储，按需读取）
```

### 4.3 范围查询流程

```
rangeQuery(field, lo=T1, hi=T2):
  1. 从根节点递归：
     - 若子树 max < lo 或 min > hi → 剪枝，跳过整棵子树
     - 若子树完全包含在 [lo,hi] → 整块收集 doc_id
     - 否则递归进入子树
  2. 叶块内 SIMD 线性扫描，收集满足条件的 doc_id
  3. 输出：DocIdSet（有序 vector 或 Roaring Bitmap）
  4. DocIdSet 与全文检索 posting list 做 AND 合并

复杂度：O(√N + k)，N 为文档数，k 为结果数
对比 Phase 1 O(N) 全列扫描：百万文档时约快 1000x
```

### 4.4 作为 Pre-filter 的查询流程

```
查询：pubtime:[2024-01-01, 2025-01-01] AND text:"python"

                   BKD rangeQuery(pubtime, T1, T2)
                          ↓
                   DocIdSet {3, 7, 15, 89, ...}   ← 满足时间范围的文档集
                          ↓ AND
           text "python" posting list iterator
                          ↓
                   交集 doc_id → BM25 打分 → TopK
```

---

## 五、v2 整体模块归属

```
src/
├── fastfield/
│   ├── fast_field_writer.cpp    写入：flush 时按列追加
│   ├── fast_field_reader.cpp    读取：O(1) 随机访问 + O(N) 全列扫描
│   └── bkd_tree.cpp             Phase 2：BKD 构建 + 范围查询
├── query/
│   ├── index_searcher.cpp       扩展：支持数值过滤参数
│   └── posting_iterator.cpp     新增：惰性 posting 迭代器（缺口1修复）
└── core/
    └── index_writer.cpp         扩展：addDocument 提取数值字段
```

```
include/
├── fastfield/
│   ├── fast_field_writer.h
│   ├── fast_field_reader.h
│   └── bkd_tree.h
└── query/
    └── posting_iterator.h
```

新增 Segment 文件：

```
_N.ff_pubtime    FastField 列存（int64，定长）
_N.ff_uid        FastField 列存（int64，定长）
_N.ff_pagerank   FastField 列存（float，定长）
_N.bkd_pubtime   BKD Tree 范围索引（Phase 2）
_N.bkd_uid       BKD Tree 范围索引（Phase 2）
```

---

## 六、v2 实现顺序

```
Step 1  types.h 扩展
        Document 增加 pubtime(int64) / uid(int64) 字段
        SearchRequest 结构体（含数值过滤条件）

Step 2  FastFieldWriter
        flush() 时写 _N.ff_<field>

Step 3  FastFieldReader
        readInt64Field() / readFloatField() / rangeFilter() / readAllInt64()

Step 4  IndexWriter + SegmentWriter 集成
        addDocument 提取数值字段，flush 时调用 FastFieldWriter

Step 5  IndexSearcher 扩展
        支持 SearchRequest，In-filter 模式（rangeFilter AND posting list）
        page_rank boost 参与 BM25 打分

Step 6  SegmentMerger 扩展
        merge 时重建 FastField 列（按新 doc_id 顺序重写）

Step 7  缺口修复：惰性 Posting 迭代器（BlockSegmentPostings 模式）

Step 8  BKD Tree 构建（基于 FastField 数据）

Step 9  BKD Tree Pre-filter 接入 SearchRequest

Step 10 缺口修复：BlockMaxWAND（消费 upper_bound + SkipNode.max_score）
```

---

## 七、v2 后（v3 预留方向）

- DSL 查询语言（`query-grammar/` 模块）：类 SQL 语法解析，支持 `AND/OR/NOT/RANGE/PHRASE`
- 短语查询（Phrase Query）：利用已有 `.pos` 文件，实现位置约束匹配
- Roaring Bitmap：替换 `std::vector<DocId>` 作为 DocIdSet，加速集合 AND/OR 运算
- SIMD 加速 PForDelta 解压（当前为位循环）
- `.fdt` 原文 LZ4/Zstd 压缩
