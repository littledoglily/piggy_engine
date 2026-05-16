# Schema 驱动建索引设计方案

> 讨论日期：2026-05-16  
> 当前状态：设计阶段，未实现  
> 关联文件：`include/types.h` / `src/core/index_writer.cpp` / `src/fastfield/`

---

## 一、背景与问题

当前 `IndexWriter` 的字段处理逻辑**全部硬编码**：

```cpp
// index_writer.cpp — addDocument 内部
std::string full_text = doc.title + " " + doc.body;  // 只有这两个字段建倒排
analyzer_.analyze(doc.doc_id, full_text);

FastFieldDoc ffd;
ffd.pubtime   = doc.pubtime;    // 这三个字段写 FastField
ffd.uid       = doc.uid;
ffd.page_rank = doc.page_rank;
```

存在的问题：

| 问题 | 影响 |
|------|------|
| 哪些字段建倒排是硬编码的 | 无法扩展新文本字段 |
| FastField 字段固定为 pubtime/uid/page_rank | 新数值字段需要改源码 |
| title 和 body 合并分词，无法按字段检索 | 不支持 `title:python` 语法 |
| 所有字段都写 `.fdt`，无法关闭 | 浪费存储 |
| `avg_doc_len` 全局统一 | 多字段时 BM25 不准 |

---

## 二、参考实现对比

### 2.1 Lucene

Lucene 用 `FieldType` 对每个字段独立配置，**在写入时通过代码指定**，没有独立的 schema 文件：

```java
FieldType titleType = new FieldType();
titleType.setIndexOptions(IndexOptions.DOCS_AND_FREQS_AND_POSITIONS);
titleType.setStored(true);

FieldType pubtimeType = new FieldType();
pubtimeType.setDocValuesType(DocValuesType.NUMERIC);  // 等价 FastField
pubtimeType.setIndexOptions(IndexOptions.NONE);

doc.add(new Field("title",   titleText,   titleType));
doc.add(new Field("pubtime", 1706745600L, pubtimeType));
```

**IndexOptions 枚举：**

| 值 | 含义 |
|----|------|
| `NONE` | 不建倒排 |
| `DOCS` | 只存 doc_id（布尔检索） |
| `DOCS_AND_FREQS` | doc_id + tf |
| `DOCS_AND_FREQS_AND_POSITIONS` | doc_id + tf + 位置 |
| `DOCS_AND_FREQS_AND_POSITIONS_AND_OFFSETS` | 加字符偏移（高亮用） |

**DocValuesType（等价 FastField）：**

| 值 | 含义 |
|----|------|
| `NUMERIC` | int64 列存 |
| `BINARY` | 字节数组列存 |
| `SORTED` | 字符串列存（可排序） |
| `SORTED_NUMERIC` | 多值数值列存 |

---

### 2.2 Tantivy

Tantivy 用显式 `Schema` 对象，在创建 `Index` 时一次性传入，之后**不可更改**（immutable schema）：

```rust
let mut builder = Schema::builder();

builder.add_text_field("title",
    TextOptions::default()
        .set_indexing_options(
            TextFieldIndexing::default()
                .set_index_option(IndexRecordOption::WithFreqsAndPositions)
        )
        .set_stored()
);

builder.add_u64_field("pubtime",
    NumericOptions::default().set_fast().set_stored()
);

builder.add_text_field("body", TEXT);  // 预设：倒排 + 位置，不存原文

let schema = builder.build();
let index  = Index::create_in_dir(path, schema)?;
```

字段选项**正交组合**：

```
倒排：set_indexing_options(IndexRecordOption)
         ├── Basic                  只有 doc_id
         ├── WithFreqs              doc_id + tf
         └── WithFreqsAndPositions  doc_id + tf + 位置
列存：set_fast()    → FastField（数值）
原文：set_stored()  → .store 文件
```

---

## 三、piggy_engine Schema 设计

### 3.1 Schema 文件格式（JSON）

文件存放路径：索引目录下的 `schema.json`，随索引一起持久化。

```json
{
  "fields": [
    {
      "name":   "title",
      "type":   "text",
      "index":  "freqs_positions",
      "stored": true,
      "fast":   false
    },
    {
      "name":   "body",
      "type":   "text",
      "index":  "freqs_positions",
      "stored": false,
      "fast":   false
    },
    {
      "name":   "category",
      "type":   "keyword",
      "index":  "docs_only",
      "stored": true,
      "fast":   false
    },
    {
      "name":   "source",
      "type":   "keyword",
      "index":  "docs_only",
      "stored": true,
      "fast":   false
    },
    {
      "name":   "pubtime",
      "type":   "int64",
      "index":  "none",
      "stored": false,
      "fast":   true
    },
    {
      "name":   "uid",
      "type":   "int64",
      "index":  "docs_only",
      "stored": false,
      "fast":   true
    },
    {
      "name":   "page_rank",
      "type":   "float32",
      "index":  "none",
      "stored": false,
      "fast":   true
    }
  ]
}
```

### 3.2 核心枚举与结构体

```cpp
// 倒排精度（对标 Lucene IndexOptions）
enum class IndexOption {
    None,           // 不建倒排
    DocsOnly,       // doc_id（布尔检索 / 等值过滤）
    FreqsOnly,      // doc_id + tf（BM25，无位置）
    FreqsPositions  // doc_id + tf + 位置（支持短语查询）
};

// 字段数据类型
enum class FieldType {
    Text,     // 经过 Analyzer 分词，产出 token 流
    Keyword,  // 不分词，整体作为一个 term（适合 category、url、id）
    Int64,    // 数值型，仅支持 fast / stored
    Float32   // 数值型，仅支持 fast / stored
};

// 单字段完整描述
struct FieldSchema {
    std::string  name;
    FieldType    type;
    IndexOption  index  = IndexOption::None;
    bool         stored = false;  // true → 写入 .fdt，可从结果取回
    bool         fast   = false;  // true → 写入 .ff_<name>，支持范围过滤 / 排序
};

// 整个 Schema（Index 级别，不可变）
struct Schema {
    std::vector<FieldSchema> fields;

    // 按名称查找
    const FieldSchema* find(const std::string& name) const;

    // 持久化 / 加载
    void   save(const std::string& index_dir) const;
    static Schema load(const std::string& index_dir);
    static Schema fromJson(const std::string& json_path);

    // 查询各类字段列表（IndexWriter 路由用）
    std::vector<const FieldSchema*> indexedTextFields()    const;  // text/keyword + index != none
    std::vector<const FieldSchema*> storedFields()         const;  // stored == true
    std::vector<const FieldSchema*> fastFields()           const;  // fast == true
};
```

### 3.3 Document 泛化

当前 `Document` 是强类型结构体，升级后改为字段 map：

```cpp
// 泛化后的 Document
struct FieldValue {
    std::string str_val;   // text / keyword / stored string
    int64_t     int_val = 0;
    float       flt_val = 0.0f;
};

struct Document {
    // 内部引擎 ID（不变）
    DocId    doc_id = 0;

    // 所有业务字段，key = schema 中的 field name
    std::unordered_map<std::string, FieldValue> fields;

    // 便利方法
    Document& set(const std::string& name, std::string  v);
    Document& set(const std::string& name, int64_t      v);
    Document& set(const std::string& name, float        v);
};
```

---

## 四、IndexWriter 路由逻辑

`addDocument` 按 Schema 路由每个字段：

```
addDocument(doc):

  for each field in schema:

    if field.type == Text or Keyword:
      if field.index != None:
        → tokens = analyzer.analyze(field_value)  // Text 分词，Keyword 整体
        → mem_index.addTokens(field_name, tokens)  // 字段前缀化 term：field:token
      if field.stored:
        → stored_buf[field_name] = field_value

    if field.type == Int64 or Float32:
      if field.fast:
        → ff_buf[field_name].push(field_value)
      if field.stored:
        → stored_buf[field_name] = field_value  // 数值也可以存 .fdt

  avg_doc_len 按字段独立统计（每个 text field 一个）
```

### Term 命名空间（字段前缀）

为支持 `field:query` 检索，term 写入倒排时加字段前缀：

```
当前：  "python" → posting list
升级后："title:python"  → posting list（title 字段的倒排）
        "body:python"   → posting list（body 字段的倒排）
        "_all:python"   → posting list（合并字段，向后兼容）
```

---

## 五、FastField 泛化

当前 FastField 硬编码三个字段名，升级后按 Schema 动态生成文件：

```
当前文件名：_N.ff_pubtime / _N.ff_uid / _N.ff_page_rank
升级后：    _N.ff_<field_name>  （由 Schema 中 fast=true 的字段决定）
```

`FastFieldWriter` / `FastFieldReader` 接口泛化：

```cpp
// 写入
class FastFieldWriter {
    void add(const std::string& field, int64_t val);
    void add(const std::string& field, float   val);
    void flush(const std::string& dir, uint32_t seg_id,
               const Schema& schema) const;
};

// 读取
class FastFieldReader {
    int64_t getInt64(const std::string& field, uint32_t idx) const;
    float   getFloat(const std::string& field, uint32_t idx) const;
    std::vector<uint32_t> filterInt64(
        const std::string& field, int64_t lo, int64_t hi) const;
};
```

---

## 六、与现有实现的对比

| 维度 | 现在（硬编码）| 升级后（Schema 驱动）|
|------|-------------|-------------------|
| 建倒排的字段 | `title + body`（合并）| Schema 中 `index != none` 的字段 |
| FastField 字段 | `pubtime / uid / page_rank` | Schema 中 `fast: true` 的字段 |
| 存原文字段 | 所有字段 | Schema 中 `stored: true` 的字段 |
| Keyword 字段 | 无 | `type: keyword`，不分词，整体作 term |
| 按字段检索 | 不支持 | `title:python`，`category:science` |
| `avg_doc_len` | 全局一个 | 每个 text field 独立统计 |
| Schema 持久化 | 无 | `schema.json` 存入索引目录 |
| 新增字段 | 改源码重编译 | 改 JSON 文件重建索引 |

---

## 七、实施顺序

```
Step 1  Schema 结构体 + JSON 解析
        ├── FieldSchema / Schema 类定义
        ├── 手写 JSON 解析（不引入第三方库，与 wiki_indexer 解析方式一致）
        └── Schema::save() / Schema::load() 持久化

Step 2  Schema 写入 .si 文件
        ├── SegmentWriter 接收 Schema，存入 segment 元数据
        └── SegmentReader 读取并还原 Schema（保证 Reader 不依赖外部传入）

Step 3  IndexWriter 接收 Schema，按配置路由各字段
        ├── 保持现有 Document 结构体作为过渡（向后兼容）
        ├── 通过 Schema 决定哪些字段走 Analyzer / Keyword / FastField
        └── stored 字段动态写入 .fdt

Step 4  Analyzer 按字段粒度分词
        ├── term 添加字段前缀（title:python）
        ├── 保留 _all 虚拟字段（向后兼容无字段前缀的查询）
        └── avg_doc_len 按字段独立统计

Step 5  FastFieldWriter/Reader 泛化为任意字段名
        ├── 文件名从硬编码改为 Schema 驱动
        └── NumericFilter 泛化为 field name → range

Step 6  Document 泛化为 map 结构（API 破坏性变更）
        └── wiki_indexer / wiki_searcher 同步更新

Step 7  IndexSearcher 支持 field:query 语法
        └── QueryParser 解析 "title:python body:tutorial"
```

> Step 1-3 可保持向后兼容（提供默认 Schema 映射到当前硬编码行为）。  
> Step 4 开始有 term 格式变化，已有索引需要重建。  
> Step 6 有 API 破坏性变更。

---

## 八、默认 Schema（向后兼容）

为保证现有代码和测试不受影响，提供一个与当前行为等价的默认 Schema：

```json
{
  "fields": [
    { "name": "title",     "type": "text",    "index": "freqs_positions", "stored": true,  "fast": false },
    { "name": "body",      "type": "text",    "index": "freqs_positions", "stored": false, "fast": false },
    { "name": "category",  "type": "keyword", "index": "none",            "stored": true,  "fast": false },
    { "name": "source",    "type": "keyword", "index": "none",            "stored": true,  "fast": false },
    { "name": "pubtime",   "type": "int64",   "index": "none",            "stored": false, "fast": true  },
    { "name": "uid",       "type": "int64",   "index": "none",            "stored": false, "fast": true  },
    { "name": "page_rank", "type": "float32", "index": "none",            "stored": false, "fast": true  }
  ]
}
```

---

## 九、实现记录（2026-05-16）

> 完成状态：Step 1 / Step 3 / Step 5 已实现（对应设计中的七步计划）。  
> Step 2（Schema 写入 .si）合并入 Step 1 处理方式——Schema 写到 `schema.json` 而非 .si，SegmentReader 构造时从目录加载，效果等价。  
> Step 4 / Step 6 / Step 7 尚未实现。

### 9.1 与原始设计的偏差

| 设计点 | 原始设计 | 实际实现 | 原因 |
|--------|---------|---------|------|
| Schema 写入位置 | `.si` 文件 | `schema.json`（索引目录根） | 更易读、调试友好；.si 格式已固定不便扩展 |
| SegmentWriter 签名 | 保留 `ff_docs` 参数 | 移除 `ff_docs`，FF 由 IndexWriter 直接调用 `ff_writer_.flush()` | 职责更清晰，SegmentWriter 只负责倒排和存储 |
| FastField 字段名删除 | Schema 驱动列举 | 目录通配扫描 `_N.ff_*` | 无需重新加载 Schema 即可删除旧字段文件 |
| Document 结构 | Step 3 保持具名结构体 | 同设计，通过桥接函数访问 | 与设计一致，Step 6 再做破坏性变更 |

### 9.2 关键实现细节

#### 跨字段位置偏移（Position Base）

原始设计没有提及，但实现时发现是必须解决的问题：

当 `title` 和 `body` 分别调用 `analyzer_.analyze()` 时，每次位置从 0 开始，同一 doc 的 posting 会出现重复位置，导致位置列表不单调，违反倒排格式约束。

解决方案：在 `addDocument` 内跨字段累积 `field_pos_base`：

```cpp
uint32_t field_pos_base = 0;
for (const auto& fs : schema_.fields) {
    if (fs.type == FieldType::Text && fs.index != IndexOption::None) {
        auto tokens = analyzer_.analyze(doc.doc_id, val);
        uint32_t max_pos = field_pos_base;
        for (auto& tok : tokens) {
            tok.position += field_pos_base;
            max_pos = std::max(max_pos, tok.position);
            mem_index_.addToken(tok);
        }
        field_pos_base = max_pos + 1;  // 下个字段紧接在此之后
    }
}
```

效果：`title "Multi Python"` 的 python 在 pos=1，`body "python python"` 的 python 在 pos=2、3——位置全局单调递增。

#### FastFieldWriter / Reader 泛化

```cpp
// 旧（硬编码）
std::vector<int64_t> pubtimes_;
std::vector<int64_t> uids_;
std::vector<float>   page_ranks_;

// 新（Schema 驱动）
std::map<std::string, std::vector<int64_t>> int64_fields_;
std::map<std::string, std::vector<float>>   float32_fields_;
```

文件名从 `_N.ff_pubtime` 等硬编码变为由 Schema `fast=true` 字段动态决定，格式不变（定长二进制数组）。旧的 `add(FastFieldDoc)` 和命名访问方法（`pubtime(idx)`、`uid(idx)`、`pageRank(idx)`）作为兼容层保留，内部路由到新接口。

#### FastField 在 Merger 中的处理

```cpp
// Merger Step8：按 Schema 重建 FastField
Schema schema = Schema::load(dir_);
FastFieldWriter ff_writer;
for (const auto& gd : alive_docs) {
    for (const auto* fs : schema.fastFields()) {
        if (fs->type == FieldType::Int64)
            ff_writer.addInt64(fs->name, reader->ff().getInt64(fs->name, idx));
        else if (fs->type == FieldType::Float32)
            ff_writer.addFloat32(fs->name, reader->ff().getFloat32(fs->name, idx));
    }
}
```

### 9.3 修改的文件

```
include/schema/schema.h          ← 新增
src/schema/schema.cpp            ← 新增
include/fastfield/fast_field_writer.h    ← 改
src/fastfield/fast_field_writer.cpp      ← 改
include/fastfield/fast_field_reader.h    ← 改
src/fastfield/fast_field_reader.cpp      ← 改
include/core/index_writer.h      ← 改（加 Schema 参数）
src/core/index_writer.cpp        ← 改（Schema 路由 + position base）
include/segment/segment_writer.h ← 改（移除 ff_docs 参数）
src/segment/segment_writer.cpp   ← 改（移除 FF flush step）
include/segment/segment_reader.h ← 改（暴露 ff() 方法）
src/segment/segment_reader.cpp   ← 改（加载 Schema，传给 FFReader）
src/segment/segment_merger.cpp   ← 改（Step8 + deleteSegmentFiles）
CMakeLists.txt                   ← 改（加入 src/schema/schema.cpp）
```

### 9.4 未实现（待后续步骤）

| 步骤 | 内容 | 备注 |
|------|------|------|
| Step 4 | term 加字段前缀（`title:python`）；`avg_doc_len` 按字段独立统计 | 有 term 格式变更，已有索引需重建 |
| Step 6 | Document 泛化为 `map<string, FieldValue>` | API 破坏性变更，需同步更新 wiki_indexer / wiki_searcher |
| Step 7 | IndexSearcher 支持 `field:query` 语法 | 依赖 Step 4 |
