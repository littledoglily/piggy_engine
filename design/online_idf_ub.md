# 在线计算 IDF 和 UB 改造方案

## 问题根因

当前实现在构建期（flush 时）计算 IDF 和 UB，存入 `.tim` 文件，查询期直接读取。这导致两类错误：

### 1. IDF 使用局部 N

`IndexWriter::flush()` 传给 `SegmentWriter` 的 `total_docs` 是 `stored_docs_buf_.size()`（当前 buffer 的文档数），不是全局 `total_docs_`。即使传了全局值，flush 时后续 segment 的文档还不存在，N 也是中间值。

```
// index_writer.cpp:130-131 — 传的是局部 buf size
seg_writer.flush(mem_index_, stored_docs_buf_,
                 stored_docs_buf_.size(),   // ← 局部，非全局 total_docs_
                 avg_doc_len);
```

`SegmentReader::bm25Score()` 也用 `doc_count_`（segment 局部）计算 IDF，而非全局总文档数。

### 2. UB 依赖错误 IDF

`writeTim()` 中：`ub = max_tf_norm × idf(df_local, N_local)`。N 偏小 → IDF 偏大 → UB 高估 → WAND 剪枝过于保守（漏掉本可剪的候选），或因 UB 不一致导致跨 segment 结果不可比。

### 3. PForDelta 中 SkipNode.max_score 同样错误

`PForDelta::compress` 接受 `global_idf`，计算 `BlockHeader.max_score = 0.45 × idf`（block 级 UB）。但传入的 idf 也是局部计算的。虽然 `max_score` 当前未被 WAND 消费，但日后启用时同样有问题。

---

## 改造目标

**构建期**：只存原始统计量（`df`, `ttf`），以及不依赖全局 N 的局部量（`max_tf_norm`）。  
**查询期**：`IndexSearcher` 汇总全局统计，实时计算 IDF 和 UB，注入到评分和 WAND 剪枝中。

---

## 改造方案

### 核心思路

BM25 score = `tf_norm(tf, dl, avgdl) × idf(df, N)`

- `tf_norm`：依赖 tf（segment 局部）、dl（doc 长度，局部）、avgdl（全局平均）
- `idf`：依赖 df（可跨 segment 求和）、N（全局总文档数）

当前 tf_norm 公式已简化为 `dl = avgdl`（即忽略 doc length 差异），所以 `tf_norm = tf × (k1+1) / (tf + k1)`，纯粹局部可计算。

**`upper_bound` 字段语义变更**：从 `max_tf_norm × idf`（错误）改为存 `max_tf_norm`（正确），IDF 在查询期乘入。

---

### 改动一览

| 文件 | 改动 |
|------|------|
| `src/segment/segment_writer.cpp` | 删除 `calcIdf` 调用；`calcUB` → `calcMaxTfNorm`（不乘 IDF）；`PForDelta::compress` 传 `max_score_no_idf` |
| `include/segment/segment_writer.h` | `calcIdf` 删除；`calcUB` → `calcMaxTfNorm` 签名变更 |
| `include/types.h` | `TermMeta.upper_bound` 注释：语义从 `max_bm25_contrib` 改为 `max_tf_norm` |
| `src/postings/pfor_delta.cpp` | `compress` 的 `global_idf` 参数改为 `max_tf_norm`，直接存入 `max_score` |
| `include/postings/pfor_delta.h` | `compress` 签名同步 |
| `src/segment/segment_reader.cpp` | `bm25Score` 增加 `global_total_docs` 参数；IDF 用全局 N 计算 |
| `include/segment/segment_reader.h` | `bm25Score` 签名变更 |
| `include/query/index_searcher.h` | 增加 `global_total_docs_` 成员；增加 `computeTermStats()` |
| `src/query/index_searcher.cpp` | 构造期汇总全局 doc_count；查询期计算 global_df、global_idf、per-seg UB；传给 `bm25Score` |
| `src/core/index_writer.cpp` | `flush()` 传 `total_docs_`（全局）而非 `stored_docs_buf_.size()`（局部）—— 注意：这只能修正 avg_doc_len，N 的根本修复在查询期 |

---

### 详细改动

#### 1. SegmentWriter：只存原始量

```cpp
// 原：idf × max_tf_norm
float idf = calcIdf(pl->size(), total_docs);
float ub  = calcUB(*pl, idf, avg_doc_len);

// 改：只存 max_tf_norm
float max_tf_norm = calcMaxTfNorm(*pl);
meta.upper_bound  = max_tf_norm;
```

`calcMaxTfNorm` 实现（不依赖 idf 和 N）：
```cpp
static float calcMaxTfNorm(const PostingList& pl) {
    const float k1 = 1.2f;
    float max_norm = 0.0f;
    for (const auto& e : pl.entries()) {
        float tf   = static_cast<float>(e.tf);
        float norm = tf * (k1 + 1.0f) / (tf + k1);  // dl=avgdl 简化
        max_norm   = std::max(max_norm, norm);
    }
    return max_norm;
}
```

`PForDelta::compress` 的第三参数由 `idf` 改为 `max_tf_norm`：
```cpp
// 原
hdr.max_score = 0.45f * global_idf;

// 改（存 max_tf_norm，查询期 × global_idf 得真正 block UB）
hdr.max_score = max_tf_norm;
```

`writeTim()` 不再需要 `total_docs` 参数（删去）；`writeDoc()` 同步去掉 idf 计算。

#### 2. IndexSearcher：汇总全局统计

构造期：
```cpp
uint32_t global_total_docs_ = 0;

IndexSearcher::IndexSearcher(const std::string& dir) {
    // ... 加载 segments
    for (const auto& seg : segments_) {
        global_total_docs_ += seg->docCount();
    }
}
```

查询期，每个 term 计算全局 df 和 idf：
```cpp
struct TermStats {
    uint32_t global_df;
    float    global_idf;
    float    ub_for_seg[N];  // per-seg: max_tf_norm × global_idf
};

// 在 search() 中，分发给每个 segment 前先算 term stats
for (const auto& term : terms) {
    uint32_t global_df = 0;
    for (const auto& seg : segments_) {
        const TermMeta* m = seg->getTermMeta(term);
        if (m) global_df += m->doc_freq;
    }
    float global_idf = calcGlobalIdf(global_df, global_total_docs_);
    term_idfs[term] = global_idf;
}
```

将 `term_idfs` 传给 `bm25Score` 和 WAND cursor 的 UB：
```cpp
// searchOR_WAND 中
c.ub = meta->upper_bound * term_idfs.at(t);  // max_tf_norm × global_idf
```

#### 3. SegmentReader::bm25Score 接收全局 IDF

```cpp
// 原签名
float bm25Score(DocId doc_id, const std::vector<std::string>& terms) const;

// 新签名
float bm25Score(DocId doc_id,
                const std::vector<std::string>& terms,
                const std::unordered_map<std::string, float>& term_idfs) const;
```

内部不再用 `doc_count_` 算 IDF，直接查 `term_idfs`：
```cpp
// 原
float idf = std::log(1.0f + (doc_count_ - meta->doc_freq + 0.5f) / (meta->doc_freq + 0.5f));

// 改
float idf = term_idfs.count(term) ? term_idfs.at(term) : 0.0f;
```

#### 4. IndexWriter::flush — 修正传入值

```cpp
// 原：传 buf size（局部）
seg_writer.flush(mem_index_, stored_docs_buf_,
                 stored_docs_buf_.size(),   // ← 局部
                 avg_doc_len);

// 改：total_docs_ 是全局累计值，但对于 IDF 修正意义已转移到查询期
// 此处 total_docs 仍供 SegmentInfo.doc_count 使用（该 segment 的文档数）
// 传 stored_docs_buf_.size() 是正确的，代表本 segment 文档数
// avg_doc_len 同理：用本 segment 的 total_tokens_/buf_size
// SegmentWriter 内不再用 total_docs 算 IDF，此参数可简化为 seg_doc_count
```

注：`flush()` 传入的 `total_docs` 参数在改造后只用于写 `SegmentInfo.doc_count`，不再用于 IDF 计算，逻辑上不需要改动（`stored_docs_buf_.size()` = 本 segment 文档数，是正确的）。

---

### 数据流对比

**改造前：**
```
flush() 时
  SegmentWriter::writeTim()
    idf = calcIdf(df_local, N_local)   ← N 错误
    ub  = max_tf_norm × idf            ← UB 错误
    写入 .tim: upper_bound = ub

search() 时
  SegmentReader::bm25Score()
    idf = log(1 + (doc_count_ - df + 0.5) / (df + 0.5))  ← N 仍是 segment 局部
  searchOR_WAND()
    c.ub = meta->upper_bound            ← 读错误的 UB
```

**改造后：**
```
flush() 时
  SegmentWriter::writeTim()
    max_tf_norm = calcMaxTfNorm(pl)     ← 纯局部，正确
    写入 .tim: upper_bound = max_tf_norm

search() 时
  IndexSearcher::search()
    global_total_docs = Σ seg->docCount()
    for each term:
      global_df  = Σ seg->getTermMeta(t)->doc_freq
      global_idf = log(1 + (global_total_docs - global_df + 0.5) / (global_df + 0.5))
  searchOR_WAND()
    c.ub = meta->upper_bound × global_idf  ← 正确
  bm25Score()
    idf = term_idfs[term]               ← 全局正确 IDF
```

---

## 不在本次改动范围内

- **avg_doc_len 全局化**：当前 tf_norm 公式已做 `dl=avgdl` 简化，avgdl 对分数无影响。待实现真正 doc length 归一化时再处理。
- **BlockMaxWAND 消费 SkipNode.max_score**：本次修正了 max_score 的存储语义（改为 max_tf_norm），为后续启用 Block 级剪枝打好基础，但消费逻辑不在本次范围。
- **SegmentMerger 重算 UB**：merge 后 max_tf_norm 仍来自各原始 segment，合并后的新 segment 理论上应重新扫描算 max_tf_norm。此次改为查询期算 IDF，UB 的 merge 修正可作为独立优化。

---

## 实施顺序

1. 改 `pfor_delta.h/.cpp`：参数 `global_idf` → `max_tf_norm`
2. 改 `segment_writer.h/.cpp`：删 `calcIdf`，`calcUB` → `calcMaxTfNorm`，更新 `writeTim`/`writeDoc`
3. 改 `segment_reader.h/.cpp`：`bm25Score` 增 `term_idfs` 参数
4. 改 `index_searcher.h/.cpp`：增 `global_total_docs_`，`search()` 中计算 term stats 并传递
5. 跑 `test_all` / `demo` 验证分数变化符合预期

全部完成，编译零警告，测试全绿。改动总结：

文件	改动
pfor_delta.h / .cpp	global_idf → max_tf_norm，block max_score 直接存 tf_norm
segment_writer.h / .cpp	删 calcIdf，calcUB → calcMaxTfNorm（无 IDF），upper_bound 存 max_tf_norm
segment_reader.h / .cpp	bm25Score 增 term_idfs 参数，内部不再用 doc_count_ 算 IDF
index_searcher.h / .cpp	构造期汇总 global_total_docs_；新增 computeTermIdfs()；searchAND/searchOR_WAND 接收 term_idfs；WAND UB 改为 meta->upper_bound × global_idf