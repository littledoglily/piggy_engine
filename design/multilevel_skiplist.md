# 多层级 SkipList 设计

## 现状分析

当前实现已有 2 层：
- **Level0**：每 128 个 doc（每 Block）一个节点
- **Level1**：Level0 节点数 > 128 时才建立（即 df > 16,384 才有 Level1）

`find()` 流程：先线性扫描 Level1，定位到 Level0 起始范围，再线性扫描 Level0。

```
// skiplist.cpp 当前 find()
// Step1：线性扫描 Level1 → 缩小 Level0 搜索起点
// Step2：从 l0_start 开始线性扫描 Level0
```

---

## 问题定量分析

| df（文档数） | Level0 节点 | Level1 节点 | `find()` 最坏扫描次数 |
|-------------|------------|------------|---------------------|
| 1K          | 8          | 0          | 8（无 Level1）|
| 16K         | 128        | 0          | 128（临界，Level1 未建）|
| 100K        | 782        | 7          | 7 + 128 = **135** |
| 1M          | 7813       | 62         | 62 + 128 = **190** |
| 10M         | 78125      | 611        | 611 + 128 = **739**（开始昂贵）|
| 128M        | 1M         | 7813       | 7813 + 128 = **7941**（不可接受）|

**结论**：
- df < 1M（典型 wiki segment）：2 层完全够用，find() < 200 次比较
- df > 2M（超大 segment）：Level1 本身成为线性瓶颈，需要 Level2
- Level2 触发条件：Level1 节点数 > 128，即 df > 128³ ≈ **2M**

实际 Lucene 单 Segment 上限约 2.1B docs，理论上需要 3 层。本引擎的实际 RAM buffer 限制（128MB）决定单次 flush 最多约 **数十万 docs**，2 层在可预见范围内足够。本设计作为长期演进路径记录。

---

## 设计目标

将现有固定 2 层扩展为 **动态 N 层**，层数由 df 自动决定，使任意规模下 `find()` 的比较次数保持 O(SKIP_INTERVAL) = O(128)。

---

## 核心数据结构

```cpp
// include/postings/skiplist.h

class SkipList {
public:
    static constexpr int SKIP_INTERVAL = 128;

    explicit SkipList(std::vector<SkipNode> nodes);  // 从 Level0 构建，自动建高层
    SkipList() = default;

    FindResult find(DocId target_doc_id) const;

    // 公开访问（调试/测试用）
    size_t levelCount() const { return levels_.size(); }
    size_t levelSize(int lv) const { return levels_[lv].size(); }
    const SkipNode& node(size_t i) const { return levels_[0][i]; }  // Level0 节点
    size_t size() const { return levels_.empty() ? 0 : levels_[0].size(); }
    bool   empty() const { return size() == 0; }

    // 序列化/反序列化
    std::vector<uint8_t> serialize()  const;
    static SkipList       deserialize(const uint8_t* data, size_t len);
    size_t serializedSize() const;

private:
    // levels_[0] = Level0（每 Block 一个），levels_[1] = Level1，以此类推
    std::vector<std::vector<SkipNode>> levels_;

    void buildHigherLevels();   // 从 Level0 向上逐层构建，直到顶层节点数 <= SKIP_INTERVAL
};
```

**关键变化**：原 `level0_`/`level1_` 两个独立成员合并为 `levels_` 数组，层数完全动态。

---

## 构建算法

```cpp
void SkipList::buildHigherLevels() {
    // levels_[0] 已填充（Level0）
    // 逐层向上构建，直到当前层节点数 <= SKIP_INTERVAL（不值得再建上层）
    while (levels_.back().size() > (size_t)SKIP_INTERVAL) {
        const auto& lower = levels_.back();
        std::vector<SkipNode> upper;
        upper.reserve((lower.size() + SKIP_INTERVAL - 1) / SKIP_INTERVAL);

        for (size_t i = 0; i < lower.size(); i += SKIP_INTERVAL) {
            size_t last = std::min(i + SKIP_INTERVAL - 1, lower.size() - 1);
            SkipNode sn;
            sn.max_doc_id  = lower[last].max_doc_id;
            sn.byte_offset = lower[i].byte_offset;    // 该批第一个 Level0 Block 的偏移
            sn.max_score   = 0.0f;
            for (size_t j = i; j <= last; ++j)
                sn.max_score = std::max(sn.max_score, lower[j].max_score);
            sn.doc_count   = 0;  // 高层节点不用 doc_count
            upper.push_back(sn);
        }
        levels_.push_back(std::move(upper));
    }
    // levels_ 从低到高：levels_[0]=Level0, levels_[1]=Level1, ...
}
```

**层数公式**：
```
层数 = ceil(log(Level0节点数) / log(SKIP_INTERVAL))
     = ceil(log(df/128) / log(128))
```

| df | Level0 | 层数 |
|----|--------|------|
| < 16K | < 128 | 1（无上层）|
| 16K–2M | 128–16K | 2（加 Level1）|
| 2M–256M | 16K–2M | 3（加 Level2）|

---

## 查找算法

```cpp
SkipList::FindResult SkipList::find(DocId target_doc_id) const {
    if (levels_.empty() || levels_[0].empty())
        return {UINT64_MAX, SIZE_MAX, 0};

    // 从最高层开始，逐层向下缩小搜索范围
    size_t l0_start = 0;
    int top = (int)levels_.size() - 1;

    // 在最高层找到第一个 max_doc_id >= target 的节点
    // 该节点在 levels_[top-1] 中的起始范围是 [i * SKIP_INTERVAL, ...]
    // 逐层向下展开，直到 Level0
    for (int lv = top; lv >= 1; --lv) {
        const auto& cur_level = levels_[lv];
        // 在 [l0_start_for_this_level, ...) 内线性扫描（最多 SKIP_INTERVAL 个节点）
        // l0_start 在本层的等价起点
        size_t lv_start = l0_start;  // 转换：l0_start 是相对 Level0 的偏移
                                     // 对于 lv 层，等价起点 = l0_start / SKIP_INTERVAL^lv
        size_t scale = 1;
        for (int s = 0; s < lv; ++s) scale *= SKIP_INTERVAL;
        size_t cur_start = lv_start / scale;

        size_t found_idx = cur_level.size();  // 默认超出
        for (size_t i = cur_start; i < cur_level.size(); ++i) {
            if (cur_level[i].max_doc_id >= target_doc_id) {
                found_idx = i;
                break;
            }
        }
        if (found_idx == cur_level.size())
            return {UINT64_MAX, SIZE_MAX, 0};  // target 超出范围

        // 更新下一层的搜索起点
        l0_start = found_idx * SKIP_INTERVAL;
    }

    // 在 Level0 中线性扫描（最多 SKIP_INTERVAL 个节点）
    const auto& l0 = levels_[0];
    for (size_t i = l0_start; i < l0.size() && i < l0_start + SKIP_INTERVAL; ++i) {
        if (l0[i].max_doc_id >= target_doc_id)
            return {l0[i].byte_offset, i, 0};
    }
    return {UINT64_MAX, SIZE_MAX, 0};
}
```

每层线性扫描最多 `SKIP_INTERVAL = 128` 个节点，总比较次数 = `层数 × 128`。

---

## 序列化格式变更

```
原格式（固定 2 层）:
  4B  level0_count
  4B  level1_count
  level0_count × sizeof(SkipNode)
  level1_count × sizeof(SkipNode)

新格式（变层）:
  4B  level_count              // 实际层数（通常 1-3）
  4B  level0_count
  [4B  level1_count]           // level_count >= 2 时存在
  [4B  level2_count]           // level_count >= 3 时存在
  ...
  level0_count × sizeof(SkipNode)
  level1_count × sizeof(SkipNode)   // 若存在
  level2_count × sizeof(SkipNode)   // 若存在
```

**向后兼容**：反序列化时，旧格式（level_count 字段位置存的是 level0_count）可通过值范围区分。
建议写入版本号（见下方注意事项）。

---

## 对外接口变更对比

| 接口 | 原 | 改后 |
|------|----|------|
| `level0_` | 成员变量 | `levels_[0]`，通过 `node(i)` / `size()` 访问 |
| `level1_` | 成员变量 | `levels_[1]`，若存在 |
| `levelCount()` | 无 | 新增，返回实际层数 |
| `find()` | 2 层硬编码 | 动态 N 层 |
| `serialize()` | 固定 2 段 | 动态段数，头部加 `level_count` |

`SkipList::node(i)` 和 `size()` 接口签名不变，`printTermDebug` 无需改动。

---

## 验证方案

### 单元测试（`tests/postings/test_skiplist.cpp` 扩展）

```cpp
// 1. 小规模：< 128 个 Block，仅有 Level0
auto sl_small = buildSkipList(100 /*blocks*/);
ASSERT(sl_small.levelCount() == 1);

// 2. 中规模：Level1 触发（>128 blocks = >16K docs）
auto sl_mid = buildSkipList(200 /*blocks*/);
ASSERT(sl_mid.levelCount() == 2);

// 3. 大规模：Level2 触发（>128*128 blocks = >2M docs）
auto sl_large = buildSkipList(20000 /*blocks*/);
ASSERT(sl_large.levelCount() == 3);

// 4. find() 正确性：对每个 Block 边界±1 做 find，结果与暴力线性扫描一致
for each test case: ASSERT(sl.find(target) == bruteForce(target));

// 5. 序列化往返：serialize → deserialize → find() 结果不变
auto sl2 = SkipList::deserialize(sl.serialize());
ASSERT(sl2.find(target) == sl.find(target));
```

### 性能测试

构造 df=10M 的 SkipList，对比 `find()` 的比较次数：

| 实现 | Level1 扫描 | Level0 扫描 | 总比较次数 |
|------|------------|------------|-----------|
| 当前 2 层 | 611 | 128 | **739** |
| 3 层设计 | 5 (Level2) + 128 (Level1) | 128 | **261** |
| 二分搜索（对比） | log₂(611) ≈ 10 | log₂(128) = 7 | **17** |

二分搜索更快，但增加代码复杂度；3 层 SkipList 是 Lucene/Tantivy 的工业标准做法，缓存友好且实现简单。

---

## 不在本文档范围内

- **二分搜索替代线性扫描**：可以在每层内用 `lower_bound` 替代 for 循环，实现 O(log(SKIP_INTERVAL)) 每层，但 SKIP_INTERVAL=128 时差异极小，暂不处理
- **Level1/Level2 的 max_score 用于 BlockMaxWAND**：当前 BlockMaxWAND 只用 Level0 的 max_score，高层节点的 max_score 暂保留但不消费，待 BlockMaxWAND 成熟后可扩展为跨 Block 批次剪枝
- **Segment Merge 后重建 SkipList**：merge 时需根据合并后的 posting 重新构建，层数可能变化，由 `SegmentMerger` 负责
