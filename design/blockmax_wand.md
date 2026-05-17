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

## 前置依赖

**必须先完成 `lazy_posting_iterator.md`**，因为：

1. `blockMaxScore()` 接口由 `PostingIterator` 提供（当前 Block 的 `SkipNode.max_score`）
2. Block 级跳跃需要 `iter.advance(target)` 跳到 Block 边界，而非在全量 vector 上做 `lower_bound`

无 lazy iterator 时可做退化版本（见"退化版本"节），但剪枝效果有限。

---

## 算法变更

### 原 WAND 流程（简化）

```
sort cursors by curDoc()
find pivot: first cursor where Σ(c.ub) >= θ
if minDoc == pivotDoc:
    exact_score = bm25Score(pivotDoc)
    try push to heap
    advance all cursors at pivotDoc
else:
    advance cursors before pivot to pivotDoc
```

### BlockMaxWAND 新增一步（在精确计算前）

```
sort cursors by curDoc()
find pivot: first cursor where Σ(c.block_ub) >= θ   // ← 用 block_ub，非全局 ub
if minDoc == pivotDoc:
    // ★ 新增：Block 级检验
    block_ub_sum = Σ(c.blockMaxScore() * idf) for all cursors at pivotDoc's block
    if block_ub_sum < θ:
        // 整个 Block 不可能进 TopK，跳过整个 Block
        for each cursor at pivotDoc: iter.advance(blockEnd(pivotDoc))
        continue
    
    // Block 通过检验，精确计算
    exact_score = bm25Score(pivotDoc, term_idfs)
    try push to heap
    advance all cursors at pivotDoc
else:
    advance cursors before pivot to pivotDoc
    // ★ 跨 Block advance 后，更新 cursor 的 block_ub
    for each advanced cursor: c.block_ub = c.iter.blockMaxScore() * idf
```

---

## TermCursor 结构变更

```cpp
struct TermCursor {
    std::string      term;
    PostingIterator  iter;      // 惰性迭代器（依赖 lazy_posting_iterator）
    float            list_ub;   // 整条 list 的 UB = meta->upper_bound * idf（WAND 粗筛）
    float            block_ub;  // 当前 Block 的 UB = iter.blockMaxScore() * idf（BlockMax 细筛）

    DocId curDoc() const { return iter.docId(); }

    // advance 后调用，同步更新 block_ub
    void refreshBlockUb(float idf) {
        block_ub = iter.blockMaxScore() * idf;
    }
};
```

初始化时：
```cpp
c.list_ub  = meta->upper_bound * idf;     // 构造时设置，不再变动
c.block_ub = iter.blockMaxScore() * idf;  // 每次 advance 后刷新
```

---

## 两级 UB 的使用策略

```
WAND pivot 判断使用：list_ub（整条 list，保守上界，决定是否需要精确计算）
BlockMax 剪枝使用：block_ub（当前 Block，动态更新，决定是否跳过整个 Block）
```

两级分工避免误剪：
- `list_ub` 过滤：确保 WAND 不会漏掉真正的 TopK 候选
- `block_ub` 过滤：在 WAND 认为值得看的 doc 中，进一步跳过分数不够的 Block

---

## max_score 存储语义确认

当前 `SkipNode.max_score` 和 `TermMeta.upper_bound` 存的都是 **max_tf_norm**（不含 IDF），在构建期由 `calcMaxTfNorm()` 计算（见 `online_idf_ub.md`）。

查询期：
```
block_ub = SkipNode.max_score * global_idf   // SkipNode.max_score = block 内 max_tf_norm
list_ub  = TermMeta.upper_bound * global_idf // TermMeta.upper_bound = 整个 list 的 max_tf_norm
```

**注意**：`SkipNode.max_score` 目前由 `PForDelta::compress` 写入，值为 block 内所有 doc 的 max_tf_norm。需确认 `pfor_delta.cpp` 的写入逻辑与此一致（见 `online_idf_ub.md` 对 PForDelta 的改动）。

---

## 退化版本（无 lazy iterator 时）

若 lazy iterator 尚未完成，可做以下简化版本作为过渡：

1. 仍全量加载 posting list 到 `vector<DocId>`
2. 为每个 cursor 维护 `cur_block_idx_ = ptr / 128`
3. `block_ub = skip_list.node(cur_block_idx_).max_score * idf`
4. 在 `min_doc == pivot_doc` 时，检查 `block_ub_sum < θ` 则把 ptr 推到下一个 Block 开头

退化版本不节省内存，但可以验证 Block 级剪枝的正确性，为切换到完整版本做铺垫。

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
