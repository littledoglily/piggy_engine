# BlockMaxWAND Block 级剪枝设计

## 现状与问题

当前 `searchOR_WAND` 使用整条 Posting List 的上界（`meta->upper_bound * global_idf`）作为每个 cursor 的 UB，在累加判断 `ub_sum >= θ` 时，只要 theta 不够大就无法剪枝。

核心问题：**UB 是全局最大值，不是当前 cursor 所在 Block 的最大值**。即使当前 Block 内所有 doc 的分数都远低于 θ，WAND 仍会把该 Block 的所有 doc 挨个精确计算。

```
// 当前 WAND — cursor UB 是整条 posting list 的最大值
c.ub = meta->upper_bound * idf;   // 全局 max_tf_norm × idf
                                   // 不随 Block 推进而更新
```

---

## 设计目标

引入 **BlockMaxWAND**：cursor 的 UB 动态跟随当前 Block 的 `SkipNode.max_score`，让 WAND 能在 Block 粒度做剪枝：

- 当 `ub_sum < θ` 时（累加所有 cursor 的 **Block 级** UB 仍不够），直接跳过包含 pivot 的整个 Block
- 对比现有 WAND：只能在 doc 级跳过；BlockMaxWAND：可以跳过整个 128-doc Block

---

## 前置依赖（均已完成）

1. ✅ `lazy_posting_iterator.md`：`PostingIterator` 提供 `blockMaxScore()` / `blockMaxDocId()` / `advance()`
2. ✅ per-block max_score 写入：`calcBlockMaxTfNorms()` + `PForDelta::compress(block_ubs)` 确保每 Block 存储独立的 max_tf_norm

---

## 算法变更（已实现）

### 原 WAND 流程

```
sort cursors by curDoc()
find pivot: first cursor where Σ(c.list_ub) >= θ
if minDoc == pivotDoc:
    exact_score = bm25Score(pivotDoc)
    try push to heap
    advance all cursors at pivotDoc
else:
    advance cursors before pivot to pivotDoc
```

### BlockMaxWAND 完整流程

```
sort cursors by curDoc()
find pivot: first cursor where Σ(c.list_ub) >= θ   // pivot 判断仍用 list_ub（保证不漏召回）
if minDoc == pivotDoc:
    // ★ Block 级细筛
    block_ub_sum = Σ(c.block_ub for ALL cursors)
    // 为什么用 ALL？Σ(ALL block_ub) 是任意 doc 分数的保守上界：
    //   curDoc==pivot 的 cursor：block_ub = 当前 Block 内最高贡献
    //   curDoc>pivot 的 cursor：已越过 pivot，对 pivot 贡献=0 ≤ block_ub
    if block_ub_sum < θ:
        // 当前所有 Block 内没有 doc 能进 TopK
        for each cursor: iter.advance(blockMaxDocId() + 1)  // 整个 Block 跳过
        for each cursor: refreshBlockUb()
    else:
        exact_score = bm25Score(pivotDoc, term_idfs)
        try push to heap
        for cursors at pivotDoc: iter.advance(pivotDoc + 1); refreshBlockUb()
else:
    advance cursors before pivot to pivotDoc
    for each advanced cursor: refreshBlockUb()   // 跨 Block 后刷新
```

---

## TermCursor 结构（已实现）

```cpp
struct TermCursor {
    std::string      term;
    PostingIterator  iter;
    float            idf;
    float            list_ub;   // 整条 list 的 max_tf_norm × IDF（静态，WAND pivot 用）
    float            block_ub;  // 当前 Block 的 max_tf_norm × IDF（动态，BlockMax 细筛用）

    DocId curDoc() const { return iter.docId(); }
    void refreshBlockUb() { block_ub = iter.blockMaxScore() * idf; }
};
```

---

## 两级 UB 的使用策略

```
WAND pivot 判断：list_ub（保守，不随 Block 推进变化，保证不漏召回）
BlockMax 细筛：  block_ub（激进，随 advance 动态刷新，跳过整个 Block）
```

正确性保证：
- `list_ub` 是整条 list 的上界，不会随 cursor 推进减小，WAND pivot 判断永远安全
- `Σ(ALL block_ub) < θ` → 当前所有 Block 内无 doc 能超过 θ → 整块跳过正确

---

## max_score 存储语义确认（已实现）

`SkipNode.max_score` 存储 **当前 Block 内** 的 max_tf_norm（不含 IDF）；
`TermMeta.upper_bound` 存储 **整条 list** 的 max_tf_norm。两者独立计算，语义不同。

查询期：
```
block_ub = SkipNode.max_score * global_idf   // per-block max_tf_norm × IDF
list_ub  = TermMeta.upper_bound * global_idf // 全局 max_tf_norm × IDF（WAND 粗筛）
```

### 实现路径

**`segment_writer.cpp`**：
- `calcMaxTfNorm(pl)` → 遍历全部 entries，取最大 tf_norm，写入 `TermMeta.upper_bound`
- `calcBlockMaxTfNorms(pl)` → 按 128-doc 分组，逐 Block 取最大 tf_norm，返回 `vector<float>`

**`pfor_delta.cpp`**：
- `compress(doc_ids, skip_nodes_out, block_max_tf_norms)` → 每个 Block 取 `block_max_tf_norms[blk_idx]` 写入 `BlockHeader.max_score`，再同步到 `SkipNode.max_score`
- 传空 `block_max_tf_norms`（默认参数）时每个 Block 写 0.0f（兼容旧调用路径）

**调用链**（flush 时）：
```
writeDoc()
  → block_ubs = calcBlockMaxTfNorms(*pl)      // per-block max_tf_norm
  → PForDelta::compress(doc_ids, snodes, block_ubs)
       → hdr.max_score = block_ubs[blk_idx]   // ✓ 每 Block 独立值
       → sn.max_score  = hdr.max_score
```

---

---

## 验证方案

### 正确性验证

BlockMaxWAND 是 WAND 的优化，结果集必须与原 WAND 完全一致：

```cpp
// test_blockmax_wand.cpp
for (each query in test_queries) {
    auto results_wand     = searcher.search(q, top_k, OR, /*blockmax=*/false);
    auto results_blockmax = searcher.search(q, top_k, OR, /*blockmax=*/true);
    ASSERT(results_wand == results_blockmax);  // doc_id 集合相同，分数相同
}
```

建议构造以下边界用例：
- `top_k = 1`：θ 快速升高，Block 剪枝最激进
- 单词查询（无 UB 累加剪枝机会）：退化为纯精确计算，结果不变
- theta = 0 初始时：第一次 pivot 判断前无法剪枝，验证第一个 Block 正确处理
- 仅剩最后一个 doc 在 Block 末尾：验证 Block 边界处理

### 效率验证

在 df > 50K 的高频词上，对比两个版本的：
- **精确计算次数**（bm25Score 调用次数）：BlockMaxWAND 应显著更少
- **Block 级跳跃次数**：新增计数器，验证确实有 Block 被跳过
- **查询延迟**：在真实 wiki 数据（1M+ docs）上 p50/p99

---

## 不在本文档范围内

- **Position-aware 打分**：TF 精确计算（当前 BM25 用 tf=1 简化），与本设计无交叉
- **AND 查询的 Block 剪枝**：AND 的 Zigzag 可独立引入 Block 级 `advance`，但不属于 WAND 改造
- **多 Segment 的 theta 共享**：当前各 Segment 独立维护 heap，跨 Segment 的全局 theta 传播是另一个优化方向
