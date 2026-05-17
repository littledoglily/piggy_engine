# 多字段索引构建设计

> 整理日期：2026-05-17  
> 关联文件：`include/schema/schema.h` / `src/core/index_writer.cpp` / `src/segment/segment_writer.cpp`  
> 上游设计：`design/schema_driven_index.md`（Step 1-7 实施路线）

---

## 一、现状梳理

### 1.1 字段数据类型（FieldType）

| 类型 | 枚举值 | 支持的索引操作 | 当前实现状态 |
|------|--------|--------------|------------|
| 全文文本 | `Text` | 倒排（全三种精度）、`stored` | ✅ |
| 不分词字符串 | `Keyword` | 倒排（全三种精度）、`stored` | ✅ 建索引可用；存储见 §1.3 |
| 64位整数 | `Int64` | `fast`（列存过滤/排序）、`stored` | ✅ fast；存储见 §1.3 |
| 32位浮点 | `Float32` | `fast`（列存过滤/排序）、`stored` | ✅ fast；存储见 §1.3 |

**不存在字符串列存类型**：当前没有对应 Lucene `DocValues.SORTED` / Tantivy `bytes_fast` 的字符串 FastField，字符串只能通过 `stored` 取回原文，或通过倒排查询，无法 O(1) 随机读取排序/聚合。

### 1.2 倒排精度（IndexOption）

| 精度 | 枚举值 | 存储内容 | 适用场景 |
|------|--------|---------|---------|
| 不建倒排 | `None` | — | 纯存储/列存字段 |
| 仅 doc_id | `DocsOnly` | doc_id | 布尔检索、等值过滤 |
| doc_id + tf | `FreqsOnly` | doc_id + 词频 | BM25 打分，无短语查询 |
| doc_id + tf + 位置 | `FreqsPositions` | doc_id + 词频 + 词序位置 | BM25 + 短语/邻近查询 |

**位置写入的当前 Bug**：SegmentWriter 没有 Schema 感知，`writePos()` 对 InMemoryIndex 中所有 term 都写位置。即使字段配置为 `FreqsOnly`，位置信息仍会写入 `.pos` 文件，产生无用空间开销。

### 1.3 字段值的写入路径

```
Text/Keyword 字段
  ├── index != None → Analyzer（Text 分词 / Keyword 整体作 term） → InMemoryIndex
  └── stored = true → StoredDoc（见下）

Int64/Float32 字段
  ├── fast = true → FastFieldWriter → _N.ff_<field>
  └── stored = true → ⚠️ 未实现（StoredDoc 无泛化 slot）

StoredDoc（当前硬编码结构）
  ├── doc_id, ext_id（固定）
  ├── source（URL / ISBN / 路径，固定字符串）
  ├── title（固定字符串）
  ├── body（固定字符串）
  └── category（固定字符串）
```

**核心瓶颈**：`StoredDoc` 是具名结构体，`.fdt` 序列化格式与字段名一一对应，硬编码在 `writeFdt()` / `readStoredDoc()` 中。自定义字符串字段（例如 `url`、`author`、`doi`）无法通过 Schema 配置写入 `.fdt`，必须修改源码。

---

## 二、四种索引构建场景的现状与差距

### 场景 A：字符串字段不切词建索引（Keyword 等值 / 前缀查询）

**典型用途**：URL、分类标签、ISBN、语言代码等枚举值。

**Schema 配置**：

```json
{ "name": "url",      "type": "keyword", "index": "docs_only",  "stored": true }
{ "name": "lang",     "type": "keyword", "index": "docs_only",  "stored": false }
{ "name": "category", "type": "keyword", "index": "freqs_only", "stored": true }
```

**当前状态**：

| 能力 | 状态 | 备注 |
|------|------|------|
| Keyword 建倒排 | ✅ | `addDocument` 中 Keyword 分支已实现 |
| Keyword stored 写 `.fdt` | ❌ | `StoredDoc` 没有泛化 slot，`url` 字段无法持久化 |
| Keyword stored 从结果取回 | ❌ | `readStoredDoc()` 硬编码只读固定字段 |

**需要的改动**：
- `StoredDoc` 泛化为 `map<string, string>` 存储任意字符串字段（Step 6 的一部分）
- `writeFdt()` / `readStoredDoc()` 改为读写泛化 map

---

### 场景 B：切词但不存储位置信息（Text + FreqsOnly）

**典型用途**：长正文字段，只需 BM25 打分，不需短语查询，节省 `.pos` 文件空间。

**Schema 配置**：

```json
{ "name": "body", "type": "text", "index": "freqs_only", "stored": false }
```

**当前状态**：

| 能力 | 状态 | 备注 |
|------|------|------|
| `addDocument` 路由到分词 | ✅ | Text 分支走 Analyzer，与 `FreqsPositions` 路径相同 |
| `.doc` 文件写 doc_id + tf | ✅ | PForDelta 压缩包含 tf |
| 跳过写 `.pos` | ❌ | `writePos()` 无 IndexOption 感知，所有 term 都写位置 |

**需要的改动**：
1. `InMemoryIndex` 或 `PostingList` 记录每条 posting list 对应的 `IndexOption`
2. `SegmentWriter::writePos()` 跳过 `FreqsOnly`（及 `DocsOnly`）的 term
3. `addToken()` 对 `FreqsOnly` 字段可以不填充 `positions`，节省内存

**改动影响**：需要在 `InMemoryIndex::addToken()` 时传入当前字段的 `IndexOption`，或在 `addDocument` 路由层按 IndexOption 决定是否填充 `tok.position`。

---

### 场景 C：切词且存储位置信息（Text + FreqsPositions）

**典型用途**：标题、摘要等高权重字段，支持短语查询和邻近排序。

**Schema 配置**：

```json
{ "name": "title", "type": "text", "index": "freqs_positions", "stored": true }
```

**当前状态**：✅ 完全支持，是当前默认行为。

---

### 场景 D：多个字符串字段联合切词建索引（虚拟合并字段）

**典型用途**：`title + body + abstract` 的全文检索字段 `_all`；不区分字段的宽泛搜索。

**两种语义**：

| 语义 | 含义 | 示例 |
|------|------|------|
| 隐式联合（当前行为） | 多个字段的 token 写入同一扁平 term 字典 | title 的 `python` 和 body 的 `python` 共享同一 posting list |
| 显式虚拟字段 | Schema 中定义一个 `combined` 字段，指定来源字段列表 | `{ type: combined, sources: [title, body], index: freqs_positions }` |

**当前状态（隐式联合）**：

- 由于 Step 4（term 字段前缀）尚未实现，所有 Text 字段的 token 都写入同一个扁平 InMemoryIndex，共享 posting list
- 这实际上就是联合索引行为，但不可控：所有字段强制合并，无法单独检索某个字段
- 联合分词时跨字段位置单调递增（通过 `field_pos_base` 保证），位置不会重叠

**Step 4 之后的差距**：

实现字段前缀后（`title:python`、`body:python`），隐式联合行为消失。若要继续支持无字段前缀的全局搜索，需要引入显式 `combined` 字段类型：

```json
{
  "name":    "_all",
  "type":    "combined",
  "sources": ["title", "body", "abstract"],
  "index":   "freqs_positions",
  "stored":  false
}
```

---

## 三、实现方案

### 3.1 StoredDoc 泛化（支持任意字符串字段存储）

这是场景 A 和所有 `stored: true` 字符串字段的前提。

**改动范围**：

```cpp
// 现在（include/segment/segment_writer.h）
struct StoredDoc {
    DocId       doc_id;
    uint64_t    ext_id;
    std::string source;
    std::string title;
    std::string body;
    std::string category;
};

// 目标
struct StoredDoc {
    DocId    doc_id;
    uint64_t ext_id;
    std::unordered_map<std::string, std::string> str_fields;  // 所有字符串 stored 字段
    std::unordered_map<std::string, int64_t>     int_fields;  // 数值 stored 字段（可选）
};
```

`.fdt` 格式需同步泛化（`schema_driven_index.md` Step 6 的存储层部分）：

```
当前 .fdt 每条记录：
  8B ext_id | len+str source | len+str title | len+str body | len+str category

泛化后 .fdt 每条记录：
  8B ext_id | 4B field_count | (len+str field_name | len+str field_value) × N
```

泛化后 `readStoredDoc()` 返回 `map<string, string>`，查询层按字段名取值。

**兼容策略**：旧 Segment 文件不可读（格式变更），需要重建索引。新旧格式可通过 `.si` 中的版本号区分，提供迁移路径。

---

### 3.2 IndexOption 感知的位置写入（场景 B）

**改动链**：

```
addDocument()
  ├── 查 fs.index，若 == FreqsOnly 或 DocsOnly：
  │     tok.position 不填（留 0），或完全不存入 positions 列表
  └── 若 == FreqsPositions：原有路径不变

InMemoryIndex（或 PostingList）
  └── 新增：per-term IndexOption 标记
        addToken(tok, IndexOption opt)
        PostingList::index_option → 传给 SegmentWriter

SegmentWriter::writePos()
  └── 跳过 pl.indexOption() != FreqsPositions 的 term
```

最小实现路径（不改 InMemoryIndex 内部结构）：在 `addDocument` 层对 `FreqsOnly` 字段设置 `tok.position = 0` 且不 push positions，`writePos` 自然跳过无位置的 term（`entry.positions.empty()` 时跳过该 doc）。

---

### 3.3 Document 泛化（Step 6，场景 A+D 完整支持的前提）

将 `Document` 从具名结构体改为字段 map，是支持任意字符串字段的根本解法：

```cpp
// types.h
using FieldValue = std::variant<std::string, int64_t, float>;

struct Document {
    DocId doc_id = 0;
    std::unordered_map<std::string, FieldValue> fields;

    Document& set(const std::string& name, std::string v) { fields[name] = std::move(v); return *this; }
    Document& set(const std::string& name, int64_t     v) { fields[name] = v; return *this; }
    Document& set(const std::string& name, float       v) { fields[name] = v; return *this; }
};
```

`IndexWriter::addDocument()` 直接按 `fs.name` 从 `doc.fields` 取值，不再需要桥接函数 `getDocText / getDocInt64 / getDocFloat32`。

**迁移影响**：
- `wiki_indexer.cpp` / `wiki_searcher.cpp` / 所有测试文件需同步更新
- 提供向后兼容的 `DocumentBuilder` 辅助类，保留 `.setTitle()` 等命名方法

---

### 3.4 Combined 虚拟字段（场景 D，Step 4 之后）

在 Step 4 引入字段前缀之后，增加 `FieldType::Combined`：

```cpp
// schema.h 新增
enum class FieldType {
    Text,
    Keyword,
    Int64,
    Float32,
    Combined   // 虚拟字段，合并多个 Text/Keyword 字段的 token
};

struct FieldSchema {
    std::string              name;
    FieldType                type;
    IndexOption              index   = IndexOption::None;
    bool                     stored  = false;
    bool                     fast    = false;
    std::vector<std::string> sources;  // 仅 Combined 类型使用：来源字段名列表
};
```

**addDocument 路由逻辑**：

```
for each field in schema:
    if field.type == Combined:
        combined_tokens = []
        for each src_name in field.sources:
            src_field = schema.find(src_name)
            val = doc.fields[src_name]
            if src_field.type == Text:
                combined_tokens += analyzer.analyze(doc_id, val)
            else if src_field.type == Keyword:
                combined_tokens += [Token{term=val, ...}]
        // 所有来源字段的 token 合并写入 field.name 的 posting list
        // term 加字段前缀: field.name + ":" + token.term
        for tok in combined_tokens:
            tok.term = field.name + ":" + tok.term
            mem_index.addToken(tok)
```

**与字段前缀的交互**：

```
title 字段（type:text） → 写入 "title:python"
body 字段（type:text）  → 写入 "body:python"
_all 字段（type:combined, sources:[title,body]）→ 写入 "_all:python"

查询 "python"        → 检索 "_all:python"（默认字段）
查询 "title:python"  → 检索 "title:python"
```

**Schema 示例**：

```json
{
  "fields": [
    { "name": "title",    "type": "text",     "index": "freqs_positions", "stored": true  },
    { "name": "body",     "type": "text",     "index": "freqs_only",      "stored": false },
    { "name": "url",      "type": "keyword",  "index": "docs_only",       "stored": true  },
    { "name": "category", "type": "keyword",  "index": "docs_only",       "stored": true  },
    { "name": "_all",     "type": "combined", "index": "freqs_positions",  "stored": false,
      "sources": ["title", "body"] },
    { "name": "pubtime",  "type": "int64",    "index": "none",            "fast":   true  },
    { "name": "page_rank","type": "float32",  "index": "none",            "fast":   true  }
  ]
}
```

---

## 四、实施顺序与依赖关系

```
当前已完成
  ✅ Schema 结构体 + JSON 解析（Step 1）
  ✅ IndexWriter Schema 路由（Step 3）
  ✅ FastFieldWriter/Reader 泛化（Step 5）

短期可独立实施（无依赖冲突）
  ┌─ 场景 B fix：FreqsOnly 跳过位置写入
  │   改动小，只需 addDocument 层设置 tok.position 及 SegmentWriter::writePos() 过滤
  └─ StoredDoc 泛化（场景 A 存储支持）
      需同步更新 .fdt 格式 + readStoredDoc()，有索引重建成本

中期（依赖 Document 泛化）
  └─ Step 6：Document → map<string, FieldValue>
      ├── types.h 改结构体
      ├── index_writer.cpp 移除桥接函数
      ├── wiki_indexer / wiki_searcher / 所有测试同步更新
      └── 有 API 破坏性变更

长期（依赖 Step 4 + Step 6）
  └─ Step 4：term 字段前缀（title:python）
      └─ Combined 虚拟字段（场景 D）
          └─ Step 7：IndexSearcher field:query 语法
```

---

## 五、与 Lucene 索引类型的对比

### 5.1 倒排精度（IndexOptions）

| Lucene IndexOptions | 存储内容 | piggy_engine 对应 | 差距 |
|---------------------|---------|-----------------|------|
| `NONE` | — | `IndexOption::None` | ✅ |
| `DOCS` | doc_id | `IndexOption::DocsOnly` | ✅ |
| `DOCS_AND_FREQS` | doc_id + tf | `IndexOption::FreqsOnly` | ✅（位置写入 Bug 见场景 B）|
| `DOCS_AND_FREQS_AND_POSITIONS` | doc_id + tf + 位置 | `IndexOption::FreqsPositions` | ✅ |
| `DOCS_AND_FREQS_AND_POSITIONS_AND_OFFSETS` | + 字符起止偏移 | ❌ 无 | Token 已有 `start_off/end_off`，但不写 `.pos`，高亮场景缺失 |

**字符偏移（Character Offsets）**：Lucene 在 posting list 中存储每个 term 在原文中的字节起止位置，用于 Highlighter 精确还原命中片段。piggy_engine 的 `Token` 结构体已有 `start_off / end_off` 字段，但 `writePos()` 目前未写入磁盘。

---

### 5.2 字段数据类型

#### 文本 / 字符串

| Lucene 字段类 | 语义 | piggy_engine 对应 | 差距 |
|--------------|------|-----------------|------|
| `TextField` | 分词全文，`DOCS_AND_FREQS_AND_POSITIONS` | `type:text, index:freqs_positions` | ✅ |
| `StringField` | 不分词，整体作 term，`DOCS` 或 `DOCS_AND_FREQS` | `type:keyword, index:docs_only/freqs_only` | ✅ 建索引；❌ stored 路径有 Bug |
| `StoredField`（String） | 仅存储，不建倒排 | `type:keyword/text, index:none, stored:true` | ❌ stored 字段限于硬编码 5 个名称 |
| `SortedDocValuesField` | 字符串列存，用于按字符串排序 / 聚合（有序字典压缩） | ❌ 无字符串 FastField | 字符串无法 O(1) 随机读取 |
| `SortedSetDocValuesField` | 多值字符串列存（一个 doc 多个 tag） | ❌ 无 | 多值字段场景缺失 |
| `BinaryDocValuesField` | 原始字节列存（不排序） | ❌ 无 | — |

#### 数值

| Lucene 字段类 | 语义 | piggy_engine 对应 | 差距 |
|--------------|------|-----------------|------|
| `LongPoint` / `IntPoint` | BKD 树，支持高效范围查询 | ❌ 无，FastField 用线性扫描 | 范围查询 O(N) vs BKD O(log N) |
| `FloatPoint` / `DoublePoint` | 同上，浮点 | ❌ 无 | — |
| `NumericDocValuesField` | int64 列存，用于排序 / 打分 | `type:int64, fast:true` → `_N.ff_<field>` | ✅ 功能对齐；BKD 缺失 |
| `FloatDocValuesField` | float 列存 | `type:float32, fast:true` | ✅ |
| `SortedNumericDocValuesField` | 多值 int64 列存 | ❌ 无 | — |
| `StoredField`（Long/Float） | 仅存储数值，不建倒排 | ❌ StoredDoc 无数值 slot | `stored:true` 的数值字段写不进 `.fdt` |

---

### 5.3 Lucene 有而 piggy_engine 缺失的关键能力

| 能力 | Lucene 机制 | piggy_engine 现状 | 优先级 |
|------|------------|-----------------|-------|
| **字符串列存**（排序 / 聚合） | `SortedDocValuesField` | ❌ 无 | 中（排序场景需要）|
| **BKD 数值范围索引** | `IntPoint / LongPoint` | FastField 线性扫描 O(N) | 低（当前规模可接受）|
| **多值字段** | 同一 doc 同一字段多次 add | ❌ Document 每字段仅一个值 | 中（tag / keyword 列表场景）|
| **字符偏移存储**（高亮） | `OFFSETS` IndexOption | Token 有偏移但不写磁盘 | 低（存储开销 > 20%）|
| **Field Norms** | 每 doc 存 1B 的 field length 归一化因子 | BM25 用全局 avg_doc_len 近似 | 低（近似精度够用）|
| **per-field Similarity** | 每字段可配不同打分模型 | 全局统一 BM25 | 低 |
| **Stored 数值字段** | `StoredField(Long/Float)` | ❌ StoredDoc 无数值 slot | 低 |

---

### 5.4 piggy_engine 有而 Lucene 没有的简化

| 方面 | piggy_engine | Lucene | 原因 |
|------|-------------|--------|------|
| Schema 文件 | `schema.json` 统一管理，Index 级不可变 | 无独立 schema 文件，由代码配置 | 简化外部工具集成 |
| FastField 文件格式 | 定长二进制数组，O(1) 随机读 | 分段压缩（`DirectWriter`） | 实现简单；大规模下 Lucene 更省空间 |
| UB/IDF 在线计算 | flush 时计算，WAND 剪枝直接可用 | merge 后重算 | 近似但省去 merge 后重算步骤 |

---

## 六、各场景差距一览

| 场景 | Schema 配置 | 建索引 | 存储 | 位置控制 | 缺失 |
|------|------------|--------|------|---------|------|
| A：Keyword 不切词 | `type:keyword, index:docs_only` | ✅ | ❌ stored 字段写不进 .fdt | — | StoredDoc 泛化 |
| B：Text 切词无位置 | `type:text, index:freqs_only` | ✅ | ✅/❌ | ❌ 仍写 .pos | writePos 过滤 |
| C：Text 切词有位置 | `type:text, index:freqs_positions` | ✅ | ✅ | ✅ | — |
| D：多字段联合 | `type:combined, sources:[...]` | ❌ 无此类型 | — | — | Step 4 + Combined 类型 |
| Lucene OFFSETS | `index:freqs_positions_offsets` | ❌ 无此精度 | — | — | writePos 写字符偏移 |
| 字符串列存 | `type:keyword, fast:true` | — | — | — | SortedDocValues 等价实现 |
| 多值字段 | `type:keyword_list` 等 | ❌ 无 | — | — | Document/FieldValue 支持 vector |


现在对整个读写热路径都有了清晰认识，可以给出评估。

设计方向整体合理，但有几处需要调整，改动量也比预期大。逐条分析：

好的部分
基类 + 虚接口：正确方向。消除当前散落各处的 switch(fs.type) 和 getDocText/getDocInt64/getDocFloat32 桥接函数，扩展新字段类型不需要改核心逻辑。

模板实现定长单值：合理，能复用序列化/反序列化代码。但模板参数建议用具体类型而非字节数——FixedField<int64_t> 而非 FixedField<8>，因为 4字节的 int32_t 和 float 字节数相同但算术语义完全不同，编译器无法从字节数推导类型。

需要调整的地方
1. "定长多值" 是冷门用例，不建议列为主要分类

定长多值的含义是：每个 doc 有固定数量的相同类型值（如始终是 3 个 GPS 坐标，或始终是 128 维 embedding 向量）。这是向量检索场景，与当前引擎的文本检索主线无关。

实际需要的分类只有三种：


定长单值  → int64, float32, int32 等（FastField 列存）
变长单值  → string, binary（Text/Keyword 建索引 + stored）
变长多值  → string[]（tags/authors 等多值字段，一个 doc 多个值）
"定长多值" 可以以后向量场景单独扩展，现在列进去反而增加无用复杂度。

2. Field 对象承担了两个本质不同的职责——需要拆开

用户说"通过 Field 对象封装所有信息"，但当前设计里 Field 会同时扮演两个角色：

角色	生命周期	数量
Schema 描述符（字段名/类型/索引配置）	Index 打开时创建一次，不可变	一个字段一个对象
文档字段值（addDocument 时的具体数据）	每次 addDocument 创建，处理完丢弃	每个 doc × 每个字段
如果把这两个合并成一个 Field 对象，每次 addDocument 都要构造 N 个 Field 对象来装值，100万文档 × 7字段 = 700万次对象构造，如果走堆分配会有明显开销。

建议拆成两层：


// Layer 1: 描述符（Schema 级，不可变，一次构造）
class FieldDescriptor {
    virtual std::string name()         const = 0;
    virtual IndexOption indexOption()  const = 0;
    virtual bool        isStored()     const = 0;
    virtual bool        isFast()       const = 0;
    // 派生类：FixedField<T>, VarSingleField, VarMultiField
};

// Layer 2: 文档值容器（Document 级，按字段名存）
using FieldVal = std::variant<std::string, int64_t, float, std::vector<std::string>>;
struct Document {
    DocId doc_id;
    std::unordered_map<std::string, FieldVal> fields;
};
Writer 循环：遍历 FieldDescriptor 列表，从 doc.fields[desc->name()] 取值，分派给各自的写入通道（InMemoryIndex / FastFieldWriter / stored buffer）。

3. 虚函数在读路径热点有开销

写路径（addDocument）：100K docs/s，每次虚调用完全可以接受。

读路径的两个热点：

FastFieldReader::getInt64(field, idx) —— 在 filter 循环里每个 doc 调用一次，目前是 map[field][idx]；加虚函数一层代价不大，但加了 map 名字查找就慢了，建议 Reader 侧不走虚函数，在打开 Segment 时把 FieldDescriptor → 内存数组指针绑定好，直接用指针访问。
bm25Score() 内部的 FastField 读 —— 同上。
4. 考虑不全的地方

缺失项	说明
缺失值/NULL	doc 没有某字段怎么处理？FastField 当前默认写 0，string 默认空串。需要一个 validity bitmap 或明确的 sentinel 语义
多值字段建倒排	变长多值字段（如 tags: ["ml","python"]）需要对每个值都建倒排，addDocument 的路由逻辑需要支持 vector<string> 遍历
StoredDoc 泛化	.fdt 的序列化格式目前硬编码 5 个字段，改为字段驱动后格式变更，旧 Segment 文件不兼容，需要版本号
Combined/虚拟字段	多字段联合索引（_all）需要 FieldDescriptor 有 sources 属性，addDocument 时合并多源字段的 token
IndexOption 感知位置写入	FreqsOnly 字段不应写 .pos，这个过滤点需要挂在 FieldDescriptor 上而不是 SegmentWriter 里判断
改动量估算
涉及约 10 个文件，~1500 行变更：


types.h               Document 泛化（FieldVal variant）
schema/schema.h+cpp   FieldSchema → FieldDescriptor 层级
index_writer.cpp      移除桥接函数，按描述符循环路由
segment_writer.cpp    StoredDoc → 字段驱动序列化
segment_reader.cpp    readStoredDoc → 字段驱动反序列化
fast_field_writer/reader  基本不变，接口已泛化
segment_merger.cpp    merge 时按描述符遍历字段
wiki_indexer.cpp      doc.set("title", val) 新风格
全部测试文件          Document 构建方式变化
建议
设计合理，方向正确，但建议按以下顺序实施，每步独立可编译：

Document 泛化（map<string, FieldVal>）+ 更新 addDocument 路由（移除桥接函数）—— 最高价值、改动可控
FieldDescriptor 层级 + schema.h 改造 —— 替换现有 FieldSchema
StoredDoc 泛化 —— .fdt 格式变更，有重建索引成本
变长多值字段 + Combined 虚拟字段 —— 最后实施，依赖前三步
"定长多值"建议不列入当前路线，等向量检索场景明确后再设计。