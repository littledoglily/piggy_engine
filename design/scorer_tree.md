# Scorer 树重构设计

## 一、背景与目标

### 现状

`IndexSearcher` 当前将查询执行硬编码为两条固定路径：

```
search(query, mode=AND) → searchAND()     // Zigzag 交集，flat FieldTerm 列表
search(query, mode=OR)  → searchOR_WAND() // WAND TopK，flat FieldTerm 列表
```

局限性：
- 不支持同一查询中混用 MUST / SHOULD / MUST_NOT
- 不支持子查询嵌套（如 `(body:python OR body:numpy) AND title:tutorial`）
- AND 和 WAND 的公共逻辑（upper bound、IDF、打分）分散在两个函数中无法复用
- `BlockMaxWAND` 虽已有 `SkipNode::max_score` 数据，但未能在树结构下被子节点自然暴露

### 目标

引入 **Scorer 树**：所有查询节点实现同一接口，可任意嵌套，`IndexSearcher` 只需驱动根节点。

```
BooleanQuery(+body:python +title:tutorial -source:spam)
       │
       ▼  createScorer()
ConjunctionScorer            ← MUST 节点（AND）
  ├── TermScorer(body, python)
  ├── TermScorer(title, tutorial)
  └── ExclusionScorer        ← MUST_NOT 节点（过滤）
        └── TermScorer(source, spam)

BooleanQuery(body:python body:numpy title:tutorial)   // 全 SHOULD
       │
       ▼
WANDScorer(top_k=10)         ← SHOULD 节点（OR TopK）
  ├── TermScorer(body, python)
  ├── TermScorer(body, numpyi)
  └── TermScorer(title, tutorial)
```

---

## 二、核心接口设计

### 2.1 Scorer 抽象基类

文件：`include/query/scorer.h`

```cpp
namespace ii {

// Scorer：统一的文档迭代 + 打分接口
// 每个节点（叶节点 TermScorer / 复合节点 ConjunctionScorer / WANDScorer）
// 都实现此接口，父节点无需感知子节点的具体类型。
class Scorer {
public:
    virtual ~Scorer() = default;

    // 当前文档 ID；未 next() 前行为未定义
    virtual DocId docId() const = 0;

    // 推进到下一个候选文档（不保证一定匹配父节点语义）
    // 返回 false 表示耗尽（isEnd() == true）
    virtual bool next() = 0;

    // 跳到第一个 docId >= target 的文档
    // 返回 false 表示不存在
    virtual bool advance(DocId target) = 0;

    // 当前文档的精确得分（BM25 或子节点得分聚合）
    virtual float score() = 0;

    // 当前迭代位置后，可能出现的最高得分上界（WAND pivot 判断用）
    // 不参与 MUST_NOT 语义
    virtual float maxScore() const = 0;

    // 当前 Block 内的得分上界（BlockMaxWAND 细筛用）
    // 默认返回 maxScore()；TermScorer 覆写为 block_max_score × IDF
    virtual float blockMaxScore() const { return maxScore(); }

    // 当前 Block 末尾的 doc_id（供父节点整块跳跃）
    virtual DocId blockMaxDocId() const { return INVALID_DOC; }

    virtual bool isEnd() const = 0;

    // 不可拷贝，允许移动
    Scorer(const Scorer&)            = delete;
    Scorer& operator=(const Scorer&) = delete;
    Scorer()                         = default;
};

} // namespace ii
```

### 2.2 ScorerContext（打分所需的全局上下文）

文件：`include/query/scorer_context.h`

```cpp
struct ScorerContext {
    const SegmentReader&                          seg;
    const std::unordered_map<std::string, float>& term_idfs; // "field:term" → idf
    uint32_t                                      total_docs; // 全局文档数（IDF 分母）
    const NumericFilter*                          filter;     // nullptr = 无过滤
};
```

---

### 2.3 TermScorer（叶节点）

文件：`include/query/term_scorer.h` / `src/query/term_scorer.cpp`

```cpp
class TermScorer : public Scorer {
public:
    // field="" 表示 legacy 模式（合并词典）
    TermScorer(const std::string& field,
               const std::string& term,
               const ScorerContext& ctx);

    DocId docId()         const override;
    bool  next()                override;
    bool  advance(DocId target) override;
    float score()               override;  // 调用 ctx.seg.bm25Score(field, {term}, ctx.term_idfs)
    float maxScore()      const override;  // TermMeta::upper_bound × IDF
    float blockMaxScore() const override;  // iter_.blockMaxScore() × IDF
    DocId blockMaxDocId() const override;  // iter_.blockMaxDocId()
    bool  isEnd()         const override;

private:
    PostingIterator iter_;
    std::string     field_;
    std::string     term_;
    float           idf_;
    float           list_ub_;   // = TermMeta::upper_bound × IDF（静态，构造时计算）
    const ScorerContext* ctx_;
};
```

**关键实现细节：**
- `score()` 每次调用 `ctx_->seg.bm25Score()`，避免在 advance 过程中重复计算
- `blockMaxScore()` 直接读 `iter_.blockMaxScore() × idf_`（随 Block 变化而更新）
- 构造时调用 `next()` 使迭代器指向第一个文档（与当前 `PostingIterator` 行为一致）

---

### 2.4 ConjunctionScorer（AND 节点）

文件：`include/query/conjunction_scorer.h` / `src/query/conjunction_scorer.cpp`

```cpp
class ConjunctionScorer : public Scorer {
public:
    // children 按 maxScore() 升序排列（df 越小 maxScore() 越大，但 UB 最小的先剪）
    // 实际按 df 升序（最短 posting list 做 lead）
    explicit ConjunctionScorer(std::vector<std::unique_ptr<Scorer>> children);

    DocId docId()         const override;
    bool  next()                override;
    bool  advance(DocId target) override;
    float score()               override;  // Σ children[i]->score()
    float maxScore()      const override;  // min(children[i]->maxScore())
                                           // AND 语义：只能拿最小上界
    float blockMaxScore() const override;  // min(children[i]->blockMaxScore())
    DocId blockMaxDocId() const override;  // min(children[i]->blockMaxDocId())
    bool  isEnd()         const override;

private:
    // Zigzag AND 内核
    bool doNext(DocId after);  // 推进到第一个所有子节点都命中的文档 > after

    std::vector<std::unique_ptr<Scorer>> children_;
    DocId cur_doc_ = INVALID_DOC;
};
```

**算法（doNext）：**
```
lead = children_[0]  // df 最小，构造时已排序

loop:
  target = lead.docId()
  for i in 1..N-1:
    if children_[i].advance(target) fails → exhausted, return false
    if children_[i].docId() != target:
      lead.advance(children_[i].docId())  // 追赶
      goto loop
  // 所有子节点对齐 → 命中
  cur_doc_ = target
  return true
```

---

### 2.5 WANDScorer（OR TopK 节点）

文件：`include/query/wand_scorer.h` / `src/query/wand_scorer.cpp`

```cpp
class WANDScorer : public Scorer {
public:
    WANDScorer(std::vector<std::unique_ptr<Scorer>> children,
               int top_k,
               const ScorerContext& ctx);

    // 注意：WANDScorer 不符合单步 next() 语义
    // 外部通过 collectTopK() 一次性收集结果（见下文）
    // next() / advance() 在 WANDScorer 内部作为私有推进方法使用

    // 对外接口：收集 Top-K 结果
    std::vector<SearchResult> collectTopK();

    // Scorer 接口（供父节点嵌套时使用）
    DocId docId()         const override;
    bool  next()                override;
    bool  advance(DocId target) override;
    float score()               override;
    float maxScore()      const override;  // Σ children[i]->maxScore()（OR 语义）
    float blockMaxScore() const override;  // 当前 pivot doc 的 Σ blockMaxScore()
    bool  isEnd()         const override;

private:
    // WAND pivot 选择：从左到右累加 list_ub 直到超过 theta
    int   findPivot(float theta) const;

    // 对齐所有 cursor[0..pivot] 到 pivot_doc
    bool  alignToPivot(int pivot_idx, DocId pivot_doc);

    // BlockMax 细筛：当前 pivot 的 block_ub_sum < theta 时整块跳跃
    void  skipBlock(DocId pivot_doc);

    std::vector<std::unique_ptr<Scorer>> children_; // 按 docId 排序的 cursor 集合
    float  theta_  = 0.f;   // 当前竞争分（Top-K heap 最小值）
    int    top_k_;
    DocId  cur_doc_ = INVALID_DOC;
    float  cur_score_ = 0.f;
    const ScorerContext* ctx_;

    // Min-heap for top-k
    struct HeapEntry { float score; DocId doc_id;
                       bool operator<(const HeapEntry& o) const { return score > o.score; } };
    std::priority_queue<HeapEntry> heap_;
};
```

**WAND 主循环（collectTopK 内部）：**
```
loop:
  sort cursors by docId ascending
  remove exhausted cursors

  pivot_idx = findPivot(theta)
  if not found → break

  pivot_doc = cursors[pivot_idx].docId()

  if cursors[0].docId() == pivot_doc:
    block_ub_sum = Σ cursors[i].blockMaxScore()
    if block_ub_sum < theta:
      skipBlock(pivot_doc)   // 整 Block 跳跃（BlockMaxWAND）
    else:
      score = Σ children.score() for pivot_doc
      if isAlive(pivot_doc) && passesFilter(pivot_doc):
        if score > theta:
          heap.push({score, pivot_doc})
          if heap.size() > top_k: heap.pop()
          theta = heap.top().score
      advance all cursors beyond pivot_doc
  else:
    advance cursors[0..pivot_idx-1] to pivot_doc
```

---

### 2.6 ExclusionScorer（MUST_NOT 节点）

文件：`include/query/exclusion_scorer.h` / `src/query/exclusion_scorer.cpp`

```cpp
class ExclusionScorer : public Scorer {
public:
    ExclusionScorer(std::unique_ptr<Scorer> main,
                    std::unique_ptr<Scorer> excluded);

    DocId docId()         const override;
    bool  next()                override;  // 跳过 excluded 命中的 doc
    bool  advance(DocId target) override;
    float score()               override;  // main_->score()（excluded 不贡献分数）
    float maxScore()      const override;  // main_->maxScore()
    bool  isEnd()         const override;

private:
    bool skipExcluded(DocId cur);  // 推进到第一个不在 excluded 中的 doc

    std::unique_ptr<Scorer> main_;
    std::unique_ptr<Scorer> excluded_;
    DocId cur_doc_ = INVALID_DOC;
};
```

---

### 2.7 Query 层（查询表示）

文件：`include/query/query.h`

```cpp
// Scorer 的工厂：每个 Query 知道如何为一个 Segment 创建对应的 Scorer
class Query {
public:
    virtual ~Query() = default;
    virtual std::unique_ptr<Scorer> createScorer(const ScorerContext& ctx) const = 0;
    virtual std::string debugString() const = 0;
};

// 叶节点：单 term 查询
class TermQuery : public Query {
public:
    TermQuery(std::string field, std::string term);
    std::unique_ptr<Scorer> createScorer(const ScorerContext& ctx) const override;
    std::string debugString() const override; // "field:term"

private:
    std::string field_;
    std::string term_;  // 已 stem
};

// 布尔子句
enum class Occur { MUST, SHOULD, MUST_NOT };

struct BooleanClause {
    std::unique_ptr<Query> query;
    Occur                  occur;
};

// 复合节点
class BooleanQuery : public Query {
public:
    void add(std::unique_ptr<Query> q, Occur occur);

    // 根据子句类型自动选择 Scorer 实现：
    //   全 MUST          → ConjunctionScorer
    //   全 SHOULD        → WANDScorer
    //   MUST + MUST_NOT  → ExclusionScorer(ConjunctionScorer, ...)
    //   MUST + SHOULD    → ConjunctionScorer（SHOULD 提分但不过滤）
    std::unique_ptr<Scorer> createScorer(const ScorerContext& ctx) const override;
    std::string debugString() const override;

private:
    std::vector<BooleanClause> clauses_;
};
```

**`BooleanQuery::createScorer` 路由逻辑：**

```
must_scorers   = [q.createScorer(ctx) for q in MUST clauses]
should_scorers = [q.createScorer(ctx) for q in SHOULD clauses]
must_not_scorers = [q.createScorer(ctx) for q in MUST_NOT clauses]

if must_scorers.empty() and should_scorers non-empty:
    root = WANDScorer(should_scorers, top_k, ctx)

elif must_scorers non-empty:
    if must_scorers.size() == 1:
        root = must_scorers[0]
    else:
        root = ConjunctionScorer(must_scorers)
    // MUST_NOT 包裹在外层
    if must_not_scorers non-empty:
        filter = DisjunctionScorer(must_not_scorers)  // 简单 OR，无需 TopK
        root = ExclusionScorer(root, filter)

return root
```

---

### 2.8 QueryParser 升级

文件：`include/query/query_parser.h` / `src/query/query_parser.cpp`

扩展当前 `parseQuery()` 的语法：

| 语法 | Occur | 示例 |
|------|-------|------|
| `term` | SHOULD（当前默认） | `python` |
| `+term` / `+field:term` | MUST | `+python` |
| `-term` / `-field:term` | MUST_NOT | `-spam` |
| `field:term` | SHOULD（当前 AND 模式时为 MUST） | `body:python` |

```cpp
class QueryParser {
public:
    explicit QueryParser(const Analyzer& analyzer,
                         const std::vector<std::string>& default_fields);

    // 将查询字符串解析为 BooleanQuery 树
    // 示例："+body:python language -source:spam"
    //   → BooleanQuery(
    //       MUST:    TermQuery(body, python),
    //       SHOULD:  BooleanQuery(SHOULD: TermQuery(body, language),
    //                             SHOULD: TermQuery(source, language)),
    //       MUST_NOT: TermQuery(source, spam)
    //     )
    std::unique_ptr<BooleanQuery> parse(const std::string& raw) const;

private:
    // 解析单个 token → (Occur, field, term)
    struct Token { Occur occur; std::string field; std::string term; };
    Token parseToken(const std::string& tok) const;

    const Analyzer&            analyzer_;
    std::vector<std::string>   default_fields_;
};
```

---

### 2.9 IndexSearcher 重构

重构后 `search()` 不再区分 AND/OR，而是由 `BooleanQuery` 自动路由：

```cpp
// 新签名（兼容旧签名）
std::vector<SearchResult> IndexSearcher::search(
    const std::string& query,
    int top_k,
    QueryMode mode   // 保留：mode=AND 时 bare term 作 MUST，mode=OR 时作 SHOULD
) const;

// 内部实现
std::vector<SearchResult> IndexSearcher::searchImpl(
    const BooleanQuery& bq,
    int top_k,
    const SegmentReader& seg,
    const NumericFilter* filter
) const {
    // 1. 计算 IDF（同现有 computeFieldTermIdfs）
    ScorerContext ctx{seg, term_idfs, global_total_docs_, filter};

    // 2. 创建 Scorer 树
    auto root_scorer = bq.createScorer(ctx);

    // 3. 判断根节点类型
    if (auto* wand = dynamic_cast<WANDScorer*>(root_scorer.get())) {
        // WANDScorer 内部直接收集 Top-K
        return wand->collectTopK();
    }

    // 4. ConjunctionScorer / TermScorer / ExclusionScorer：驱动迭代
    std::vector<SearchResult> results;
    while (!root_scorer->isEnd()) {
        DocId did = root_scorer->docId();
        if (seg.isAlive(did)) {
            float score = root_scorer->score();
            // 填充 SearchResult...
            results.push_back(...);
        }
        root_scorer->next();
    }
    // Top-K 截断 + 排序
    ...
    return results;
}
```

---

## 三、新增文件清单

```
include/query/
  scorer.h              Scorer 抽象基类
  scorer_context.h      ScorerContext（打分上下文）
  term_scorer.h         叶节点
  conjunction_scorer.h  AND 节点
  wand_scorer.h         OR TopK 节点
  exclusion_scorer.h    MUST_NOT 过滤节点
  query.h               Query / TermQuery / BooleanQuery / BooleanClause / Occur
  query_parser.h        QueryParser

src/query/
  term_scorer.cpp
  conjunction_scorer.cpp
  wand_scorer.cpp
  exclusion_scorer.cpp
  boolean_query.cpp
  query_parser.cpp
  index_searcher.cpp    （重构，复用上述组件）
```

不引入新模块，不影响现有 `segment/`、`postings/`、`fastfield/` 等模块。

---

## 四、依赖步骤与实施顺序

```
Step 1  Scorer 抽象接口 + ScorerContext
        新建 scorer.h / scorer_context.h
        纯头文件，无实现
        依赖：types.h，无其他依赖

Step 2  TermScorer
        wraps PostingIterator，实现 score() / maxScore() / blockMaxScore()
        依赖：Step 1，PostingIterator，SegmentReader::bm25Score(field,...)

Step 3  ConjunctionScorer（AND）
        Zigzag 逻辑从 searchAND 内联迁移至此
        依赖：Step 1

Step 4  WANDScorer（OR TopK）
        WAND 主循环从 searchOR_WAND 迁移，同时接入 blockMaxScore() 细筛
        依赖：Step 1，Step 2（blockMaxScore 来自 TermScorer）

Step 5  ExclusionScorer（MUST_NOT）
        依赖：Step 1

Step 6  Query 层（TermQuery / BooleanQuery）
        BooleanQuery::createScorer() 路由到 Step 2-5 的节点
        依赖：Step 2-5

Step 7  QueryParser 升级
        解析 +/-/field:term → BooleanQuery 树
        可向后兼容：无 +/- 前缀时行为等同现有 parseQuery()
        依赖：Step 6

Step 8  IndexSearcher 重构
        search() → QueryParser::parse() → BooleanQuery::createScorer() → 驱动根节点
        保留旧 searchAND / searchOR_WAND 接口（标记 [[deprecated]]）供过渡期使用
        依赖：Step 6-7

Step 9  BlockMaxWAND 接入（可选优化）
        WANDScorer 已接收 blockMaxScore()（Step 4），
        此步验证 SkipNode::max_score 写入的正确性并补充 Merge 时重算
        依赖：Step 4，SegmentMerger（TODO：Merge 时 UB 重算）

Step 10 测试 & 基准
        依赖：Step 1-9
```

**每步均可独立编译，不破坏现有测试。建议每步完成后运行 `./build/test_all`。**

---

## 五、验证方法

### Step 2 验证：TermScorer 正确性

```cpp
// tests/query/test_term_scorer.cpp
// 构造：相同 field/term，TermScorer 与 PostingIterator 遍历结果一致

TermScorer scorer("body", "python", ctx);
auto pl = seg.readPostingList("body", "python");  // 全量解压基准

std::vector<DocId> got;
while (!scorer.isEnd()) { got.push_back(scorer.docId()); scorer.next(); }
ASSERT(got == pl);

// maxScore() 等于 TermMeta::upper_bound × IDF
auto* meta = seg.getTermMeta("body", "python");
ASSERT(std::abs(scorer.maxScore() - meta->upper_bound * idf) < 1e-5f);
```

### Step 3 验证：ConjunctionScorer == searchAND

```cpp
// 两种实现在同一索引上执行相同查询，交集 doc_id 集合必须完全一致
auto old_results = searcher.searchAND(fterms, idfs, 1000, seg, nullptr);
auto new_results = searcher.searchImpl(boolQuery_must, 1000, seg, nullptr);

std::set<DocId> old_ids, new_ids;
for (auto& r : old_results) old_ids.insert(r.doc_id);
for (auto& r : new_results) new_ids.insert(r.doc_id);
ASSERT(old_ids == new_ids);
```

### Step 4 验证：WANDScorer Top-K 召回率

WAND 是剪枝算法，Top-K 内的结果必须与暴力枚举完全一致（不漏召回）：

```cpp
// 暴力枚举所有 OR 命中文档，取分数最高的 K 个
auto brute = bruteForceOR(fterms, seg, K);

// WANDScorer
auto wand = WANDScorer(cursors, K, ctx).collectTopK();

// Top-K 集合完全相同（允许同分时顺序不同）
ASSERT(topKDocIds(brute, K) == topKDocIds(wand, K));
```

### Step 7 验证：QueryParser 语义

```cpp
QueryParser parser(analyzer, {"body", "title"});

// 纯 SHOULD
auto q1 = parser.parse("python language");
// → BooleanQuery(SHOULD: body:python, SHOULD: title:python,
//                SHOULD: body:languag, SHOULD: title:languag)

// MUST + MUST_NOT
auto q2 = parser.parse("+body:python -source:spam");
// → BooleanQuery(MUST: TermQuery(body,python), MUST_NOT: TermQuery(source,spam))

// 检查 debugString()
ASSERT(q2->debugString() == "(+body:python -source:spam)");
```

### Step 8 验证：端到端等价

```cpp
IndexSearcher searcher(dir);

// 旧接口
auto old_or  = searcher.search("python tutorial", 10, QueryMode::OR);
auto old_and = searcher.search("python tutorial", 10, QueryMode::AND);

// 重构后（内部走 Scorer 树）
auto new_or  = searcher.search("python tutorial", 10, QueryMode::OR);
auto new_and = searcher.search("python tutorial", 10, QueryMode::AND);

ASSERT(extractIds(old_or)  == extractIds(new_or));
ASSERT(extractIds(old_and) == extractIds(new_and));
```

### Step 9 验证：BlockMax 正确性 + 性能

```cpp
// 正确性：关闭 / 开启 BlockMax 结果相同
WANDScorer no_bm  = WANDScorer(cursors, K, ctx, /*use_block_max=*/false);
WANDScorer yes_bm = WANDScorer(cursors, K, ctx, /*use_block_max=*/true);
ASSERT(no_bm.collectTopK() == yes_bm.collectTopK());

// 性能：BlockMax 跳过的 Block 数 > 0
ASSERT(yes_bm.blocksSkipped() > 0);  // 统计计数器
```

---

## 六、向后兼容策略

| 兼容场景 | 处理方式 |
|---------|---------|
| 现有 `search(query, mode=AND/OR)` | 保留签名；AND 时所有子句设为 MUST，OR 时设为 SHOULD，内部走 Scorer 树 |
| `searchAND` / `searchOR_WAND` 直接调用 | 标记 `[[deprecated]]`，实现委托给 Scorer 树 |
| 旧 `FieldTerm` 结构 | 保留；`QueryParser::parse()` 也可接受旧 `vector<FieldTerm>` 转换为 BooleanQuery |
| 无 +/- 前缀查询字符串 | 全部视为 SHOULD（OR 模式）或 MUST（AND 模式），与现有行为完全一致 |

---

## 七、实施后可继续的扩展

1. **PhraseQuery**：在 `TermScorer` 之上增加位置校验，利用 `.pos_<field>` 实现短语搜索
2. **FunctionScoreQuery**：在 Scorer 树叶节点注入 FastField 数值（如 PageRank）加权
3. **MultiSegmentScorer**：当前 `IndexSearcher` 按 Segment 串行搜索，可封装为跨 Segment 的 Scorer 树实现并行
4. **字段 Boost**：`TermScorer` 构造时传入 `field_boost` 系数，乘入 `maxScore()` 和 `score()`
5. **DisjunctionMaxScorer（DisMax）**：取子节点最高分而非累加，用于 `CombinedField` 等场景
