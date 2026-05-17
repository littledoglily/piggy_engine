# 多字段独立倒排索引文件方案

> 整理日期：2026-05-17
> 前置阅读：`design/multi_field_index_build.md`（场景分析）、`design/schema_driven_index.md`（Step 1-4 改造）
> 涉及文件：`types.h` / `field_descriptor.h` / `index_writer.*` / `segment_writer.*` / `segment_reader.*` / `segment_merger.cpp` / `index_searcher.*`

---

## 一、问题陈述

当前所有可索引字段共享同一个 `InMemoryIndex`（本质是 `map<term, PostingList>`），Token 中无字段标识。

```
文档 1：title="python 编程"，body="java python 比较"

addDocument 后 InMemoryIndex 内容：
  ["python"] → posting: [doc1(tf=2)]   ← title 和 body 的 python 合并了
  ["编程"]   → posting: [doc1(tf=1)]
  ["java"]   → posting: [doc1(tf=1)]
```

直接后果：

| 问题 | 说明 |
|------|------|
| `title:python` 查询无法实现 | term 词典中无字段维度 |
| IDF 失真 | `python` 的 df=1，实际应是 title-df 和 body-df 各自独立 |
| BM25 打分不准 | avg_doc_len 按所有字段合并计算，body 长字段拉低 title 权重 |
| FreqsOnly 字段仍写位置 | `writePos()` 无字段粒度控制，按 term 盲写 |

---

## 二、方案对比

### 方案 A：Term 前缀（Tantivy 风格）

在 `Token.term` 中嵌入字段前缀，如 `"title\x00python"`，所有字段仍共享单文件。

**优点：** 改动最小，不增加文件数量。

**缺点：**
- 词典条目膨胀 N 倍（字段数 × term 数）
- BM25 打分中 tf 查找需拼接字段前缀，侵入打分逻辑
- 跨字段合并 IDF 时逻辑复杂
- 调试可读性差

### 方案 B：per-field 独立文件（本文选择）

每个可索引字段输出独立文件集：`_N.tim_<field>`、`_N.doc_<field>`、`_N.pos_<field>`。

**优点：**
- 与现有 `_N.ff_<field>` 命名模式完全一致（FastField 已有先例）
- IDF / avg_doc_len 可按字段独立统计，BM25 更准确
- FreqsOnly 字段直接不创建 `.pos_<field>` 文件，逻辑简洁
- Merge 逻辑与单字段完全对称，逐字段独立 merge

**缺点：**
- 文件数量增加（每个 Segment M 字段 × 3 个文件）
- 改动面广（7 个核心文件），但每处改动边界清晰

**结论：选择方案 B。** 命名一致性 + 字段语义完整隔离 + 为后续 per-field BM25 打基础。

---

## 三、数据结构改造

### 3.1 `types.h` — Token 增加 field 成员

```cpp
struct Token {
    std::string term;
    std::string field;   // 新增：产生该 token 的字段名（与 FieldSchema.name 对应）
    DocId       doc_id;
    Pos         position;
    uint32_t    start_off, end_off;
};
```

所有构造 Token 的代码（`field_descriptor.h`、测试文件）需同步设置 `tok.field`。

### 3.2 `field_descriptor.h` — buildTokens 写 field

`VarSingleField::buildTokens` 构造 Token 时补充：

```cpp
tok.field = name_;   // 在每个 out.push_back(tok) 之前设置
```

`VarMultiField::buildTokens` 同理。

`CombinedField::buildTokensDoc` 中：

```cpp
tok.field = name_;   // CombinedField 自身名字作为独立字段空间
```

CombinedField 的 `name_`（如 `content`）与其 `sources_`（如 `title`、`body`）是不同的字段命名空间，`content` 有自己独立的 `_N.tim_content`，不与 `title`/`body` 合并。

### 3.3 `IndexWriter` — 按字段分桶的 InMemoryIndex

`include/core/index_writer.h` 变更：

```cpp
// 旧
InMemoryIndex mem_index_;
uint64_t      total_tokens_ = 0;

// 新
std::map<std::string, InMemoryIndex> field_indexes_;       // field → InMemoryIndex
std::map<std::string, uint64_t>      field_token_counts_;  // 用于 per-field avg_doc_len
```

`src/core/index_writer.cpp` — addDocument 路由改为：

```cpp
for (auto& tok : token_buf) {
    // tok.field 已由 descriptor 设置
    field_indexes_[tok.field].addToken(tok);
    ++field_token_counts_[tok.field];
}
```

flush 触发条件：

```cpp
bool anyBufferedData() const {
    for (const auto& [_, idx] : field_indexes_)
        if (idx.termCount() > 0) return true;
    return false;
}
```

flush() 传参变更：

```cpp
// 计算 per-field avg_doc_len
std::map<std::string, float> field_avg_doc_lens;
for (const auto& [field, cnt] : field_token_counts_)
    field_avg_doc_lens[field] = total_docs_ > 0
        ? static_cast<float>(cnt) / total_docs_ : 0.f;

seg_writer.flush(field_indexes_, stored_docs_buf_,
                 static_cast<uint32_t>(stored_docs_buf_.size()),
                 field_avg_doc_lens, schema_);

// flush 后重置
field_indexes_.clear();
field_token_counts_.clear();
```

---

## 四、文件格式设计

### 4.1 新文件命名规则

```
_N.tim_<field>    字段词典（格式与现有 .tim 完全相同）
_N.doc_<field>    字段 Posting List（格式与现有 .doc 完全相同）
_N.pos_<field>    字段位置信息（仅 FreqsPositions 字段存在，FreqsOnly 无此文件）
```

与 FastField 命名完全对称：

```
_0.ff_pubtime       ← 已有
_0.tim_title        ← 新增
_0.doc_title        ← 新增
_0.pos_title        ← 新增（title 为 FreqsPositions）
_0.tim_body         ← 新增
_0.doc_body         ← 新增
                    ← body 若为 FreqsOnly 则无 .pos_body
_0.tim_content      ← 新增（CombinedField）
_0.doc_content      ← 新增
_0.pos_content      ← 新增
```

**文件内容格式不变**，只是文件名增加字段后缀。`writeTim`/`writeDoc`/`writePos` 函数体本身几乎不需要改动，只修改路径生成逻辑。

### 4.2 `.si` 文件更新

增加 `indexed_fields` 列表，供 `SegmentReader` 构造时快速发现字段文件：

```
// 当前 .si：4B segment_id | 4B doc_count | 4B term_count | str created_at
// 新增 str indexed_fields（逗号分隔），若无则回退扫目录

segment_id:     0
doc_count:      1000
indexed_fields: title,body,content
created_at:     2026-05-17T12:00:00
```

兼容策略：`.si` 无 `indexed_fields` 时扫描目录中 `_N.tim_*` 文件；无任何 `_N.tim_<field>` 时（旧 Segment），回退加载单个 `_N.tim`（legacy mode）。

---

## 五、接口变更详述

### 5.1 `SegmentWriter`

`include/segment/segment_writer.h` 变更：

```cpp
// flush 新签名
SegmentWriteStats flush(
    const std::map<std::string, InMemoryIndex>& field_indexes,
    const std::vector<StoredDoc>&               stored_docs,
    uint32_t                                    total_docs,
    const std::map<std::string, float>&         field_avg_doc_lens,
    const Schema&                               schema     // 判断 FreqsOnly 跳过 pos
);

// per-field 文件路径辅助
std::string fieldPath(const std::string& field, const std::string& ext) const;
// → dir_ + "/_" + seg_id_ + "." + ext + "_" + field
// 例：./idx/_0.tim_title

// SegmentWriteStats 新增 per-field 统计
struct FieldIndexStats {
    uint32_t term_count  = 0;
    uint64_t tim_bytes   = 0;
    uint64_t doc_bytes   = 0;
    uint64_t pos_bytes   = 0;   // 0 表示 FreqsOnly（无 .pos 文件）
    float    avg_doc_len = 0.f;
};
std::map<std::string, FieldIndexStats> field_stats;  // 加入 SegmentWriteStats
```

`src/segment/segment_writer.cpp` 主要改动：

`flush()` 改为遍历 `field_indexes`，对每个字段依次写文件：

```cpp
for (const auto& [field, idx] : field_indexes) {
    if (idx.termCount() == 0) continue;
    std::map<std::string, TermMeta> term_dict;
    writeFieldTim(field, idx, term_dict, stats);
    writeFieldDoc(field, idx, term_dict, stats);

    const FieldSchema* fs = schema.find(field);
    bool write_pos = fs && (fs->index == IndexOption::FreqsPositions);
    if (write_pos) writeFieldPos(field, idx, term_dict);
}
```

私有方法签名（接受 field 参数，内部用 `fieldPath(field, "tim")` 取路径）：

```cpp
void writeFieldTim(const std::string& field, const InMemoryIndex& idx,
                   std::map<std::string, TermMeta>& dict_out, SegmentWriteStats& stats);
void writeFieldDoc(const std::string& field, const InMemoryIndex& idx,
                   std::map<std::string, TermMeta>& dict, SegmentWriteStats& stats);
void writeFieldPos(const std::string& field, const InMemoryIndex& idx,
                   std::map<std::string, TermMeta>& dict);
```

### 5.2 `SegmentReader`

`include/segment/segment_reader.h` 内部存储变更：

```cpp
// 旧
std::map<std::string, TermMeta>  term_dict_;
mutable std::ifstream            doc_file_;
mutable std::ifstream            pos_file_;
float avg_doc_len_ = 0.f;

// 新
std::map<std::string,
         std::map<std::string, TermMeta>> field_term_dicts_;         // field → {term → TermMeta}
mutable std::map<std::string, std::ifstream> doc_files_;             // field → .doc_<field> 句柄
mutable std::map<std::string, std::ifstream> pos_files_;             // field → .pos_<field> 句柄
std::map<std::string, float>                 field_avg_doc_lens_;
std::vector<std::string>                     indexed_field_names_;   // 有效字段列表
```

新增公开 API：

```cpp
// 字段级查询
const TermMeta*           getTermMeta(const std::string& field,
                                       const std::string& term) const;
PostingIterator           postingIterator(const std::string& field,
                                          const std::string& term) const;
std::vector<PostingEntry> readPosEntries(const std::string& field,
                                          const std::string& term) const;

// 字段列表（用于 query 展开）
const std::vector<std::string>& indexedFieldNames() const;

// 字段级 BM25
float bm25Score(DocId doc_id,
                const std::string& field,
                const std::vector<std::string>& query_terms,
                const std::unordered_map<std::string, float>& term_idfs) const;

// 向后兼容（deprecated，遍历所有字段取第一个命中）
[[deprecated]] const TermMeta* getTermMeta(const std::string& term) const;
[[deprecated]] PostingIterator postingIterator(const std::string& term) const;
```

`src/segment/segment_reader.cpp` 构造函数替换 `loadTim()` 为：

```cpp
void SegmentReader::loadPerFieldTims() {
    // 1. 从 .si 读 indexed_fields，若无则扫目录匹配 _N.tim_* 文件名
    // 2. 对每个 field：解析 _N.tim_<field> 到 field_term_dicts_[field]
    //                   打开 _N.doc_<field> 存入 doc_files_[field]
    //                   若 _N.pos_<field> 存在则存入 pos_files_[field]
    // 3. 若无任何 per-field 文件，尝试 legacy _N.tim（字段名用 ""）
}
```

### 5.3 `IndexSearcher`

查询解析层新增：

```cpp
// 字段作用域 + 分析后 term
struct FieldTerm {
    std::string field;   // "" 表示 bare term（搜所有字段）
    std::string term;    // Analyzer 分析后的 term
};

// "title:python machine" → [{field="title",term="python"}, {field="",term="machine"}]
std::vector<FieldTerm> parseQuery(const std::string& raw_query) const;
```

IDF 计算变更：

```cpp
// key: "field:term"，bare term 展开为各字段单独计算
using FieldTermIdfs = std::unordered_map<std::string, float>;
FieldTermIdfs computeFieldTermIdfs(const std::vector<FieldTerm>& fterms) const;
```

`searchAND` / `searchOR_WAND` 接受 `vector<FieldTerm>`，内部调用 `seg.postingIterator(field, term)`。

bare term 展开策略：

```
bare term "python"（field=""）展开为：
  - AND 模式：python 出现在所有 default 字段中
  - OR 模式：python 出现在任意 default 字段中
  - default 字段 = Schema 中 index != None 的 Text/Keyword/Combined 字段列表
  - IndexSearcher 构造时从第一个 Segment 的 indexedFieldNames() 初始化
```

### 5.4 `SegmentMerger`

当前 `doMerge()` 处理单字段 InMemoryIndex，改为：

```cpp
// 1. 收集所有 Segment 的 indexedFieldNames()，取并集得到全字段列表
// 2. 对每个 field 独立执行 merge：
//    a. 所有 Segment 的 field_term_dicts_[field] 的 term 取并集
//    b. 每个 term：遍历各 Segment 读 PostingList，分配新 doc_id，合并
//    c. 构建 per-field InMemoryIndex，调用 writeFieldTim/Doc/Pos
// 3. StoredDoc / fdt / fdx 部分逻辑不变
```

---

## 六、改动依赖关系与执行顺序

```
Step 1  types.h
        Token 增加 field 成员
        影响：所有构造 Token 的代码

Step 2  field_descriptor.h
        buildTokens / buildTokensDoc 写 tok.field
        依赖：Step 1

Step 3  index_writer.h / index_writer.cpp
        mem_index_ → field_indexes_，field_token_counts_
        flush() 传 per-field map + schema
        依赖：Step 1-2

Step 4  segment_writer.h / segment_writer.cpp
        flush() 新签名（per-field map + Schema）
        fieldPath() 辅助
        writeFieldTim/Doc/Pos 内部函数
        FreqsOnly 字段跳过写 pos
        .si 写 indexed_fields
        依赖：Step 3

Step 5  segment_reader.h / segment_reader.cpp
        per-field 词典加载（loadPerFieldTims）
        per-field 文件句柄
        新 API：getTermMeta(field, term) 等
        per-field bm25Score
        legacy 模式兼容（_N.tim）
        依赖：Step 4

Step 6  segment_merger.cpp
        per-field merge 主循环
        依赖：Step 4-5

Step 7  index_searcher.h / index_searcher.cpp
        parseQuery → vector<FieldTerm>
        computeFieldTermIdfs
        searchAND/OR 路由到 seg.postingIterator(field, term)
        default_search_fields_ 初始化
        依赖：Step 5

Step 8  测试 & tools 更新
        test_posting_iterator / test_merger 等中 Token 构造补 .field
        wiki_indexer / wiki_searcher search API 调整
        依赖：Step 1-7
```

每步均可独立编译通过（在向后兼容接口保留期间）。建议每步完成后运行 `./build/test_all` 确认无回归。

---

## 七、backward compatibility 策略

| 场景 | 处理方式 |
|------|---------|
| 旧 Segment（有 `_N.tim`，无 `_N.tim_<field>`） | SegmentReader 检测：`_N.tim` 存在则 legacy 模式加载，字段名设为 `""` |
| legacy 与新 Segment 混合打开 | IndexSearcher 同时持有两类 SegmentReader，新走字段路由，legacy 走全局词典 |
| `defaultSchema()` 的 title/body | 均为 Text，自动分别产生 `_N.tim_title` 和 `_N.tim_body` |
| CombinedField | 产生独立的 `_N.tim_content`，sources 字段各自也有独立文件 |
| 旧接口 `getTermMeta(term)` | 遍历 `field_term_dicts_` 取第一个命中，标记 `[[deprecated]]` |

---

## 八、受影响文件清单

| 文件 | 变更类型 | 核心改动 |
|------|---------|---------|
| `include/types.h` | 修改 | Token 增加 field 成员 |
| `include/schema/field_descriptor.h` | 修改 | buildTokens / buildTokensDoc 写 tok.field |
| `include/core/index_writer.h` | 修改 | mem_index_ → field_indexes_，field_token_counts_ |
| `src/core/index_writer.cpp` | 修改 | addDocument 路由，flush 传 per-field map |
| `include/segment/segment_writer.h` | 修改 | flush 新签名，FieldIndexStats，fieldPath |
| `src/segment/segment_writer.cpp` | 修改 | per-field 写 tim/doc/pos，FreqsOnly 跳过 pos，.si 更新 |
| `include/segment/segment_reader.h` | 修改 | per-field 词典，per-field 文件句柄，新 API |
| `src/segment/segment_reader.cpp` | 修改 | loadPerFieldTims，per-field bm25Score，legacy 兼容 |
| `src/segment/segment_merger.cpp` | 修改 | per-field merge 主循环 |
| `include/query/index_searcher.h` | 修改 | FieldTerm，parseQuery，computeFieldTermIdfs |
| `src/query/index_searcher.cpp` | 修改 | query 解析，字段路由，bare term 展开 |
| `tests/*/test_*.cpp` | 修改 | Token 构造补充 .field，search API 调整 |
| `tools/wiki_indexer.cpp` | 修改 | search 调用适配 |

---

## 九、测试计划

```cpp
// tests/segment/test_per_field_index.cpp（新建）

// 1. 写入验证：两个字段（title FreqsPositions + body FreqsOnly）
//    确认生成：_0.tim_title, _0.doc_title, _0.pos_title
//             _0.tim_body,  _0.doc_body（无 _0.pos_body）

// 2. 字段隔离：同一 term 在两个字段中 df 各自独立
//    seg.getTermMeta("title", "python").doc_freq == 1
//    seg.getTermMeta("body",  "python").doc_freq == 2
//    两个值不合并

// 3. 字段查询：
//    search("title:python") 只匹配 title 中含 python 的 doc
//    search("python")       展开为 title OR body，均命中

// 4. Merge 验证：mergeAll() 后新 Segment 仍有 per-field 文件且 df 正确

// 5. legacy 兼容：旧格式 Segment（_0.tim 无字段后缀）可正常打开和查询
```

---

## 十、实施后可继续的 TODO

1. **per-field BM25**：`bm25Score` 使用 `field_avg_doc_lens_[field]` 后，可进一步支持 per-field `k1`/`b` 参数配置。
2. **字段 Boost**：title 字段命中得分高于 body，在 IDF 层或 BM25 层乘以 boost 系数。
3. **`field:query` 完整语法**：`"title:(python java)"`、`"+title:python -body:java"` 布尔表达式。
4. **Merge 时 per-field UB 重算**：SkipNode.max_score 按字段 IDF 重算。
