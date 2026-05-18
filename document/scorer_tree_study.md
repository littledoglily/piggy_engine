# Scorer 树原理学习笔记

## 一、背景：为什么需要 Scorer 树

现有 `IndexSearcher` 硬编码两条路径：

```
search(query, mode=AND) → searchAND()      // Zigzag 交集
search(query, mode=OR)  → searchOR_WAND()  // WAND TopK
```

局限性：
- 不支持同一查询中混用 MUST / SHOULD / MUST_NOT
- 不支持子查询嵌套
- AND 和 WAND 公共逻辑分散，无法复用

**Scorer 树的解法**：所有查询节点实现同一接口，可任意嵌套，`IndexSearcher` 只需驱动根节点。

---

## 二、统一接口

所有节点（叶子或复合）都实现 `Scorer`：

```cpp
class Scorer {
    virtual DocId docId()         const = 0;  // 当前文档
    virtual bool  next()                = 0;  // 推进到下一个
    virtual bool  advance(DocId target) = 0;  // 跳到 >= target
    virtual float score()               = 0;  // 当前文档得分
    virtual float maxScore()      const = 0;  // 得分上界（WAND 剪枝用）
    virtual float blockMaxScore() const = 0;  // 当前 Block 上界（BlockMaxWAND 用）
    virtual bool  isEnd()         const = 0;
};
```

父节点调用子节点的 `advance(target)` 时，不需要感知子节点是 TermScorer 还是 WANDScorer，这是树结构的核心价值。

---

## 三、节点类型

| 节点 | 职责 |
|------|------|
| `TermScorer` | 叶子节点，封装一个 field 下一个 term 的 PostingIterator |
| `ConjunctionScorer` | MUST 交集，N 路 Zigzag AND |
| `WANDScorer` | SHOULD OR TopK，WAND 主循环 |
| `ExclusionScorer` | MUST_NOT 过滤，包在最外层 |

---

## 四、建树过程

### 4.1 QueryParser → BooleanQuery

| 语法 | Occur |
|------|-------|
| `term`（裸词） | SHOULD（OR 模式）或 MUST（AND 模式）|
| `+term` / `+field:term` | MUST |
| `-term` / `-field:term` | MUST_NOT |
| `field:term` | 锁定字段，不展开 |

裸词会展开到所有 `default_fields`（如 `["body", "title"]`）：

```
OR 模式：python → SHOULD: TQ(body,python), SHOULD: TQ(title,python)
AND 模式：python → MUST:   BooleanQuery(SHOULD: TQ(body,python), SHOULD: TQ(title,python))
```

### 4.2 BooleanQuery.createScorer() 路由

```
有 MUST?
  ├─ 否 → WANDScorer(全 SHOULD)
  └─ 是 → ConjunctionScorer(MUST 子句)
           SHOULD 子句懒评分（不过滤）
           有 MUST_NOT?
             └─ 是 → ExclusionScorer 包在最外层
```

---

## 五、树的形态

以 `default_fields = ["body", "title"]`，5 个 token：`[iphone17, expens, batteri, life, charg]` 为例。

### OR 模式（全 SHOULD）→ 2 层，完全扁平

```
WANDScorer(top_k=10)
  ├── TermScorer(body,  iphone17)   UB=0.95
  ├── TermScorer(title, iphone17)   UB=0.90
  ├── TermScorer(body,  expens)     UB=0.40
  ├── TermScorer(title, expens)     UB=0.38
  ├── TermScorer(body,  batteri)    UB=0.35
  ├── TermScorer(title, batteri)    UB=0.33
  ├── TermScorer(body,  life)       UB=0.20
  ├── TermScorer(title, life)       UB=0.19
  ├── TermScorer(body,  charg)      UB=0.18
  └── TermScorer(title, charg)      UB=0.17
```

### AND 模式（全裸词）→ 3 层

每个 token 必须出现（MUST），但出现在哪个字段不限（字段间 OR）：

```
ConjunctionScorer                          ← 第 1 层：term 间求交集
  ├── WANDScorer(body:iphone17, title:iphone17)  ← 第 2 层：字段间 OR
  │     ├── TermScorer(body,  iphone17)          ← 第 3 层：叶子
  │     └── TermScorer(title, iphone17)
  ├── WANDScorer(body:expens,   title:expens)
  ├── WANDScorer(body:batteri,  title:batteri)
  ├── WANDScorer(body:life,     title:life)
  └── WANDScorer(body:charg,    title:charg)
```

### 混合：`"body:iphone17 expens"`（AND 模式，field:term + 裸词）

```
ConjunctionScorer                          ← 第 1 层
  ├── TermScorer(body, iphone17)           ← 直接叶子（field 已指定，不展开）
  └── WANDScorer                           ← 裸词展开到多字段
        ├── TermScorer(body,  expens)
        └── TermScorer(title, expens)
```

等价于：`(body:iphone17) AND (body:expens OR title:expens)`

ConjunctionScorer 调用 WANDScorer.advance(target) 时，WANDScorer 内部对两个字段取 OR，外层无需感知细节。

### 带 MUST_NOT：`"+body:iphone17 expens -source:spam"`

```
ExclusionScorer                            ← 第 1 层：最外层过滤
  ├── main: ConjunctionScorer              ← 第 2 层
  │     ├── TermScorer(body, iphone17)     ← 第 3 层
  │     └── WANDScorer                    ← 第 3 层
  │           ├── TermScorer(body,  expens)
  │           └── TermScorer(title, expens)
  └── excluded: TermScorer(source, spam)   ← 第 2 层
```

---

## 六、树的深度规律

| 情况 | 根节点 | 深度 |
|------|--------|------|
| 全 `field:term` AND | ConjunctionScorer → TermScorer | 2 层 |
| 全裸词 OR | WANDScorer → TermScorer | 2 层 |
| `field:term` + 裸词 AND | ConjunctionScorer → {TermScorer \| WANDScorer} | 2 层 |
| 全裸词 AND | ConjunctionScorer → WANDScorer → TermScorer | 3 层 |
| 带 MUST_NOT | ExclusionScorer → 上述结构 | +1 层 |

**叶子永远是 TermScorer，字段聚合永远是 WANDScorer，term 间交集永远是 ConjunctionScorer。**

---

## 七、MUST_NOT 为什么放最外层

直觉上"先和某个 term 求差集得到短链"听起来可以减少后续工作，但实际相反。

以 `+body:iphone17 +expens -source:spam` 为例：

```
iphone17 posting list: 1000 docs
expens   posting list: 5000 docs
spam     posting list: 200 docs
iphone17 ∩ expens     = 50 docs
50 docs 中属于 spam   ≈ 3 docs
```

**方案 A（当前：先交集，再过滤）：**
```
ConjunctionScorer → 输出 50 个候选
ExclusionScorer   → 对 50 个候选各做一次 spam.advance()
总代价：Zigzag AND 遍历 + 50 次 advance()
```

**方案 B（先和 iphone17 求差集）：**
```
ExclusionScorer(iphone17, spam) → 遍历 1000 个 doc，跳过 spam
输出 ≈ 980 个 doc
再和 expens 求交集 → 输出 50 个候选
总代价：1000 次 spam.advance() + Zigzag AND 遍历
```

方案 B 做了更多 advance()，因为过滤放在交集之前，候选集还未被压缩。

**根本原因**：MUST_NOT 不贡献正向候选，只能事后打孔；真正压缩候选集的是 MUST 的交集。正确顺序：先缩小（MUST）→ 再打分（SHOULD）→ 最后打孔（MUST_NOT）。

Lucene 的设计与此完全一致，是经过验证的最优顺序。

---

## 八、多个 MUST term 的执行顺序

### ConjunctionScorer 是 N 路 Zigzag，不是两两嵌套

`+A +B +C +D`（df 升序：A=100, B=500, C=1000, D=5000）

**错误理解（两两嵌套二叉树）：**
```
ConjunctionScorer(
  ConjunctionScorer(
    ConjunctionScorer(A, B), C), D)
```

**实际结构（扁平 N 路）：**
```
ConjunctionScorer([A, B, C, D])  ← A 做 lead（df 最小）
  ├── TermScorer(A)  df=100   ← lead
  ├── TermScorer(B)  df=500
  ├── TermScorer(C)  df=1000
  └── TermScorer(D)  df=5000
```

### Zigzag 执行过程

```
lead = A（df 最小，驱动迭代）

round 1:  target = A@doc10
          B.advance(10) → doc10 ✓
          C.advance(10) → doc25 ✗ → lead.advance(25) → A@doc30

round 2:  target = A@doc30
          B.advance(30) → doc30 ✓
          C.advance(30) → doc30 ✓
          D.advance(30) → doc45 ✗ → lead.advance(45) → A@doc50

round 3:  target = A@doc50
          B.advance(50) → doc50 ✓
          C.advance(50) → doc50 ✓
          D.advance(50) → doc50 ✓ → 命中！
```

**没有中间结果**，不存在"先算 A∩B 得到短链再和 C 求交"。所有节点同时参与，任何一个 advance() 失败都直接让 lead 追赶（O(log N) 跳表查询）。

### 为什么最短 df 做 lead？

lead 越短：每次以 `lead.docId()` 为 target，其他节点 advance() 命中率越高，lead 自己被迫 advance() 的次数越少。用最长链（D, df=5000）做 lead，产生的 target 密集，其他节点频繁 advance() 但命中率低，总代价更高。

---

## 九、SHOULD 的执行顺序

### MUST + SHOULD 共存时

SHOULD 不过滤候选，只对 MUST 已命中的文档懒评分：

```
for each doc_X that ConjunctionScorer outputs:
    score = A.score(X) + B.score(X) + ...      // MUST 得分
    for each SHOULD term S:
        if S.advance(X) && S.docId() == X:
            score += S.score(X)                // 命中则加分，未命中跳过
```

SHOULD 的顺序对结果无影响，每个 SHOULD term 独立检查、独立加分。

### 纯 SHOULD（WANDScorer）时

顺序是动态的，每轮按 docId 升序重排 cursors，findPivot 从 UB 最大的 cursor 开始累加：

```
每轮：
  cursors 按 docId 升序重排
  findPivot(θ)：从左累加 UB 直到超过 θ → 确定 pivot_doc
  若 cursors[0].docId() == pivot_doc → 精确算分
  否则 → advance 前面的 cursors 到 pivot_doc
```

高 UB 的 term 天然在 pivot 判断中优先，但排列每轮随 docId 变化，不是静态固定的。

---

## 十、完整执行语义对比

| 子句类型 | 执行结构 | 顺序决定因素 | 是否过滤候选 |
|---------|---------|------------|------------|
| 多个 MUST | N 路 Zigzag，扁平 | df 升序，最短链做 lead | 是（最强压缩） |
| MUST + SHOULD | SHOULD 懒评分 | 无关，独立加分 | 否 |
| 纯 SHOULD | WAND，动态排列 | 每轮按 docId，UB 决定 pivot | 否（TopK 剪枝） |
| MUST_NOT | ExclusionScorer 最外层 | 在 MUST 交集之后执行 | 是（事后打孔） |
