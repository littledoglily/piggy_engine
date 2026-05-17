# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Run

```bash
# 配置（首次或 CMakeLists.txt 变更后执行）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug    # 开发阶段
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release  # 性能测试

# 编译所有目标
cmake --build build -j4

# 编译单个目标
cmake --build build --target test_schema
cmake --build build --target wiki_indexer
```

## 测试

```bash
# 运行所有模块测试（每个测试独立可执行）
./build/test_all                 # 集成测试（Analyzer + PForDelta + Merger + Searcher）
./build/test_analyzer
./build/test_pfor_delta
./build/test_skiplist
./build/test_posting_list
./build/test_posting_iterator    # PostingIterator 惰性迭代器
./build/test_merger
./build/test_fast_field
./build/test_schema
./build/test_per_field_index     # per-field 倒排索引端到端测试（Step 8）

# 运行 Demo（256 篇文档写入 + 多种查询展示）
./build/demo
```

测试框架为自研轻量宏（`tests/test_utils.h`），无第三方依赖：

```cpp
TEST(name);  // 打印测试名
PASS();      // 标记通过
FAIL(msg);   // assert(false) + 打印原因
```

每个测试文件 `main()` 结束后返回 0 表示全部通过。

## 工具

```bash
# 构建 Wiki 索引（JSONL 格式）
./build/wiki_indexer --input <wiki_dir> --output ./wiki_index --schema my_schema.json --ram 256

# 交互式检索
./build/wiki_searcher --index ./wiki_index --mode OR --top 10

# 单次查询（裸词）
./build/wiki_searcher --index ./wiki_index --query "machine learning" --mode AND --top 5

# 单次查询（field:term 语法，只检索 body 字段）
./build/wiki_searcher --index ./wiki_index --query "body:neural network" --mode OR --top 5

# 混合查询（field:term + 裸词）
./build/wiki_searcher --index ./wiki_index --query "body:python language" --mode AND --top 5
```

`wiki_indexer` Schema 加载优先级：`--schema 文件` → 输出目录中的 `schema.json`（续建） → `defaultSchema()`。

**查询语法（`wiki_searcher` / `IndexSearcher`）：**
- 裸词（`python`）：OR 模式 → 任意字段命中；AND 模式 → 所有索引字段都必须包含
- `field:term`（`body:python`）：只在指定字段的 term 空间搜索，与模式无关
- 混用：`"body:python language"` → `body:python` 限定字段，`language` 展开到所有字段

---

## 架构概览

**核心数据流：**

```
Document（map<string, FieldVal>）
  └─ IndexWriter.addDocument()
       ├─ 按 FieldDescriptor 描述符列表路由每个字段
       │   ├─ VarSingleField (Text)    → Analyzer → InMemoryIndex（倒排 + 位置）
       │   ├─ VarSingleField (Keyword) → 整体作单个 term → InMemoryIndex
       │   ├─ VarMultiField            → vector<string> 每值独立 token
       │   ├─ CombinedField            → 联合多个源字段分词 → 共享倒排
       │   ├─ FixedField<T> (fast)     → FastFieldWriter（列存缓冲）
       │   └─ VarSingleField (stored)  → stored_docs_buf_（.fdt 缓冲）
       └─ RAM 超阈值 → flush() → SegmentWriter + FastFieldWriter::flush()

查询：
IndexSearcher
  └─ 对所有 Segment 执行 searchAND/searchOR_WAND
       └─ mergeTopK 跨 Segment 归并
```

**模块职责：**

| 模块 | 关键文件 | 职责边界 |
|------|---------|---------|
| `schema` | `schema.h/cpp` | Schema 定义、JSON 持久化、字段筛选 |
| `schema` | `field_descriptor.h` | FieldDescriptor 类层级（工厂 `buildDescriptors()`） |
| `tokenizer` | `analyzer.h/cpp` | CharFilter → Tokenizer → StopFilter → Porter Stemmer |
| `postings` | `pfor_delta`, `skiplist`, `posting_list` | 内存倒排链、Block 压缩、跳表 |
| `segment` | `segment_writer/reader/merger` | Flush 文件、只读打开、合并 + 软删除 |
| `fastfield` | `fast_field_writer/reader` | 数值列存（`_N.ff_<field>`），O(1) 随机访问 |
| `core` | `index_writer` | RAM Buffer 管理、描述符路由、触发 flush |
| `query` | `index_searcher` | Zigzag AND、WAND OR、跨 Segment 归并、数值过滤 |

**Segment 文件集（每个 Segment 前缀 `_N`）：**

```
per-field 格式（当前默认，Step 4 后所有新 Segment）：
_N.tim_<field>   字段词典：term → df/ttf/UB/posting_offset/skip_offset/pos_offset
_N.doc_<field>   字段 Posting List：SkipList + PForDelta Block
_N.pos_<field>   字段位置信息（仅 FreqsPositions；FreqsOnly 字段无此文件）
_N.ff_<field>    FastField 列存（仅 fast:true 字段）

共享文件（与字段无关）：
_N.fdt   文档原文：4B doc_id | 8B ext_id | 4B field_count | (str name | str value) × N
_N.fdx   文档索引：doc_id → fdt 字节偏移
_N.liv   存活位图：软删除标记
_N.si    Segment 元数据（含 indexed_fields 字段列表）
schema.json  索引级 Schema（存于索引根目录）
```

---

## Schema 系统

Schema 在 `IndexWriter` 构建时传入，之后不可变，持久化为 `schema.json`。

```cpp
Schema schema = Schema::fromJson("my_schema.json");  // 或 Schema::defaultSchema()
IndexWriter writer(dir, ram_mb, schema);
```

**字段类型（`FieldType`）：**

| 类型 | 描述符 | 说明 |
|------|--------|------|
| `Text` | `VarSingleField` / `VarMultiField` | Analyzer 分词；`multi:true` 时值为 `vector<string>` |
| `Keyword` | `VarSingleField` / `VarMultiField` | 整体作单 term；`multi:true` 时多值独立 term |
| `Int64` | `FixedField<int64_t>` | 64-bit 整数；支持 fast / stored |
| `Float32` | `FixedField<float>` | 32-bit 浮点；支持 fast / stored |
| `Combined` | `CombinedField` | 虚拟字段：联合 `sources` 列表中的多个字段分词，不 stored / fast |

**字段属性正交组合：**

| 属性 | 效果 |
|------|------|
| `index: freqs_positions` | 建倒排 + 位置（支持短语查询） |
| `index: freqs_only` | 建倒排，仅 tf（BM25 无位置）|
| `index: docs_only` | 仅 doc_id（布尔 / 等值过滤）|
| `stored: true` | 写入 `.fdt`，可从搜索结果取回 |
| `fast: true` | 写 `.ff_<name>`，支持 O(1) 数值过滤 / 排序 |
| `multi: true` | 字段值为 `vector<string>`（`Text`/`Keyword` 适用）|
| `sources: [...]` | 源字段列表（仅 `Combined` 类型使用）|

**Schema JSON 示例（含新字段类型）：**

```json
{
  "fields": [
    {"name": "title",   "type": "text",     "index": "freqs_positions", "stored": true,  "fast": false},
    {"name": "body",    "type": "text",     "index": "freqs_positions", "stored": false, "fast": false},
    {"name": "tags",    "type": "keyword",  "index": "docs_only",       "stored": true,  "fast": false, "multi": true},
    {"name": "content", "type": "combined", "index": "freqs_positions", "stored": false, "fast": false, "sources": ["title","body"]},
    {"name": "pubtime", "type": "int64",    "index": "none",            "stored": false, "fast": true},
    {"name": "uid",     "type": "int64",    "index": "none",            "stored": false, "fast": true},
    {"name": "page_rank","type": "float32", "index": "none",            "stored": false, "fast": true}
  ]
}
```

---

## FieldDescriptor 类层级

`include/schema/field_descriptor.h` — 将 Schema 字段配置封装为可多态调用的描述符对象。

```
FieldDescriptor（抽象基类）
  ├── FixedField<T>      定长单值（int64_t / float）→ FastField 列存
  ├── VarSingleField     变长单值（Text 分词 / Keyword 整体）→ 倒排 + stored
  ├── VarMultiField      变长多值（vector<string>）→ 每值独立 token
  └── CombinedField      虚拟字段：聚合多个源字段分词 → 共享倒排
```

**核心虚方法：**

| 方法 | 说明 |
|------|------|
| `buildTokens(doc_id, val, pos_base, analyzer, out)` | 从单字段值生成 token 流 |
| `buildTokensDoc(doc_id, fields, pos_base, analyzer, out)` | 文档级入口（默认委托 `buildTokens`；`CombinedField` 重载以访问多个源字段）|
| `writeFast(val, ff_writer)` | 写 FastField 列存（`nullptr` 时写默认值 0/0.0f，保持数组对齐）|
| `storeStr(val)` | 返回 stored 字符串表示（`VarMultiField` 用 `\x1f` 分隔多值）|

**工厂函数：**

```cpp
std::vector<std::unique_ptr<FieldDescriptor>> buildDescriptors(const Schema& schema);
// Text/Keyword + multi:false → VarSingleField
// Text/Keyword + multi:true  → VarMultiField
// Int64                      → FixedField<int64_t>
// Float32                    → FixedField<float>
// Combined                   → CombinedField
```

---

## 头文件接口速查

所有头文件位于 `include/`，命名空间 `ii`。

### `types.h` — 基础类型

```cpp
// 别名
using DocId    = uint32_t;   // 1-indexed
using TermFreq = uint32_t;
using Pos      = uint32_t;
static constexpr DocId INVALID_DOC = 0xFFFFFFFFu;

// 字段值变体
using FieldVal = std::variant<std::string, int64_t, float, std::vector<std::string>>;

// 核心 POD
struct Token       { std::string term; DocId doc_id; Pos position; uint32_t start_off, end_off; };
struct PostingEntry{ DocId doc_id; TermFreq tf; std::vector<Pos> positions; };
struct SkipNode    { DocId max_doc_id; uint64_t byte_offset; float max_score; uint32_t doc_count; };
struct TermMeta    { uint32_t doc_freq, total_term_freq; uint64_t posting_offset, skip_offset, pos_offset; float upper_bound; };

// Document：字段通过 set()/get*() 操作
struct Document {
    DocId doc_id; uint64_t ext_id;
    Document& set(name, string/int64_t/float/vector<string>);  // 链式调用
    const string& getString(name) const;
    int64_t       getInt64(name, def=0) const;
    float         getFloat(name, def=0.f) const;
    const vector<string>* getStrList(name) const;
};

// 数值过滤（IndexSearcher 用）
struct NumericFilter {
    int64_t pubtime_lo, pubtime_hi; int64_t uid; bool sort_by_pubtime;
    bool hasPubtimeRange() / hasUidFilter() / hasAnyFilter() const;
};

// 搜索结果
struct SearchResult {
    DocId doc_id; float score; uint64_t ext_id; int64_t pubtime, uid;
    string source, title;                              // 向后兼容命名字段
    unordered_map<string, string> stored_fields;       // 所有 stored:true 字段
};
```

---

### `schema/schema.h` — Schema 定义

```cpp
enum class FieldType  { Text, Keyword, Int64, Float32, Combined };
enum class IndexOption{ None, DocsOnly, FreqsOnly, FreqsPositions };

struct FieldSchema {
    string name; FieldType type; IndexOption index;
    bool stored, fast, multi;
    vector<string> sources;   // Combined 专用
};

struct Schema {
    vector<FieldSchema> fields;
    const FieldSchema* find(name) const;
    vector<const FieldSchema*> indexedFields() const;  // index != None
    vector<const FieldSchema*> storedFields()  const;  // stored == true
    vector<const FieldSchema*> fastFields()    const;  // fast == true
    void         save(index_dir) const;
    static Schema load(index_dir);          // 不存在则返回 defaultSchema()
    static Schema fromJson(json_path);
    static Schema defaultSchema();
};
// 字符串转换：fieldTypeToStr/FromStr、indexOptionToStr/FromStr
```

---

### `tokenizer/analyzer.h` — 文本分析管道

```cpp
class Analyzer {
    // 文档级分析：CharFilter → Tokenizer → StopFilter → Stem → Token 列表
    vector<Token>  analyze(DocId doc_id, const string& text) const;
    // 查询级分析：返回 term 列表（不含位置）
    vector<string> analyzeQuery(const string& query) const;
};
```

---

### `postings/posting_list.h` — 内存倒排链

```cpp
class PostingList {
    void                      append(DocId, Pos);          // 同 doc 多次调用累加 tf
    vector<DocId>             docIds()  const;             // 升序，用于 PForDelta
    const vector<PostingEntry>& entries() const;
    size_t size(); bool empty(); uint32_t totalTermFreq();
};

class InMemoryIndex {
    void                  addToken(const Token&);
    vector<string>        sortedTerms() const;             // 字母序，flush 用
    const PostingList*    getPostingList(term) const;
    size_t termCount(); size_t docCount();
    void   setDocCount(n);
};
```

---

### `postings/pfor_delta.h` — Block 压缩编解码

```cpp
struct BlockHeader { uint8_t b, size, exc_count, reserved; uint32_t max_doc_id; float max_score; uint32_t first_doc_id; };
struct PatchEntry  { uint16_t position; uint32_t value; };

class PForDelta {
    static constexpr int BLOCK_SIZE = 128;
    static vector<uint8_t> compress(doc_ids, skip_nodes_out, block_max_tf_norms={});
    static vector<DocId>   decompress(data, data_len, total_docs);
    static size_t          decompressBlock(ptr, out, base_doc_id=0);  // 返回消耗字节数
    static size_t          blockByteSize(ptr);                        // 不解压，直接算
};
```

---

### `postings/skiplist.h` — Block 级跳表

```cpp
class SkipList {
    static constexpr int SKIP_INTERVAL = 128;
    explicit SkipList(vector<SkipNode> nodes);

    vector<uint8_t>  serialize() const;
    static SkipList  deserialize(data, len);

    struct FindResult { uint64_t byte_offset; size_t block_index; DocId block_first_doc; };
    FindResult       find(DocId target) const;    // O(log N)，返回 Block 偏移

    const SkipNode& node(i) const; size_t size(); bool empty();
    size_t serializedSize() const;
};
```

---

### `postings/posting_iterator.h` — 惰性迭代器

```cpp
class PostingIterator {
    PostingIterator(const TermMeta&, const string& doc_path);  // 构造后已指向第一个 doc
    PostingIterator() = default;                               // 空迭代器（isEnd()==true）
    // 不可拷贝，可移动

    DocId docId()         const;   // 当前 doc_id
    float blockMaxScore() const;   // 当前 Block 的 max_score（WAND 用）
    DocId blockMaxDocId() const;   // 当前 Block 末尾 doc_id（BlockMaxWAND 用）
    bool  isEnd()         const;

    bool next();               // 推进到下一个 doc；false 表示耗尽
    bool advance(DocId target);// 跳到第一个 >= target；false 表示不存在
};
```

---

### `fastfield/fast_field_writer.h` — 数值列存写入

```cpp
struct FFWriteStats { map<string, uint64_t> file_bytes; uint64_t total_bytes, total_us; };

class FastFieldWriter {
    void         addInt64  (field, int64_t);
    void         addFloat32(field, float);
    FFWriteStats flush(dir, seg_id) const;   // 写 _N.ff_<field>
    void         clear();
    size_t       size() const;
    void         add(const FastFieldDoc&);   // 向后兼容
};
```

---

### `fastfield/fast_field_reader.h` — 数值列存读取

```cpp
class FastFieldReader {
    FastFieldReader(dir, seg_id, doc_count, schema);   // Schema 驱动（推荐）
    FastFieldReader(dir, seg_id, doc_count);           // 向后兼容

    bool    hasField  (field) const;
    int64_t getInt64  (field, idx) const;    // O(1) 随机访问
    float   getFloat32(field, idx) const;
    vector<uint32_t> filterInt64(field, lo, hi) const;  // 范围过滤→local_doc_idx 列表

    // 向后兼容命名方法
    int64_t pubtime(idx); int64_t uid(idx); float pageRank(idx);
    vector<uint32_t> filterPubtime(lo, hi); vector<uint32_t> filterUid(uid_val);
    const vector<int64_t>& allPubtimes(); const vector<int64_t>& allUids();
    const vector<float>&   allPageRanks();

    uint32_t docCount(); bool hasData();
};
```

---

### `segment/segment_writer.h` — Flush 内存索引到磁盘

```cpp
struct SegmentWriteStats {
    uint32_t segment_id, doc_count, term_count;
    uint64_t total_pl_entries; uint32_t max_pl_df; string max_pl_term; float avg_pl_df;
    uint64_t total_skip_nodes; uint32_t max_skip_nodes; string max_skip_term;
    uint64_t tim_bytes, doc_bytes, pos_bytes, fdt_bytes, fdx_bytes, liv_bytes, si_bytes;
    uint64_t tim_us, doc_us, pos_us, fdt_fdx_us;
    uint64_t totalSegBytes(); uint64_t totalSegUs();
};

struct StoredDoc { DocId doc_id; uint64_t ext_id; unordered_map<string,string> str_fields; };

class SegmentWriter {
    explicit SegmentWriter(dir, segment_id);
    SegmentWriteStats flush(mem_index, stored_docs, total_docs, avg_doc_len);
};
```

---

### `segment/segment_reader.h` — 只读查询 Segment

```cpp
class SegmentReader {
    static constexpr uint32_t MAX_DOCS = 65536;
    explicit SegmentReader(dir, segment_id);

    // 存活管理
    bool isAlive(DocId) const;
    void softDelete(DocId);

    // 倒排查询
    const TermMeta*          getTermMeta(term) const;
    vector<DocId>            readPostingList(term) const;           // 全量解压
    vector<DocId>            readPostingListFrom(term, target) const;// 跳跃读取（AND 用）
    vector<PostingEntry>     readPosEntries(term) const;            // 带位置
    PostingIterator          postingIterator(term) const;           // 惰性迭代器（推荐）

    // 文档原文
    struct StoredDocResult {
        DocId doc_id; uint64_t ext_id;
        unordered_map<string,string> str_fields;
        const string& get(key) const;
        const string& source()/title()/body()/category() const;  // 向后兼容
    };
    StoredDocResult readStoredDoc(DocId) const;

    // BM25 打分
    float bm25Score(doc_id, query_terms, term_idfs) const;

    // FastField 数值列存
    const FastFieldReader& ff() const;
    bool hasFastField() const;
    int64_t ffPubtime(local_idx); int64_t ffUid(local_idx); float ffPageRank(local_idx);
    vector<uint32_t> filterPubtime(lo, hi); vector<uint32_t> filterUid(uid_val);

    // 元数据
    uint32_t docCount(); uint32_t termCount(); uint32_t segmentId();
    const map<string, TermMeta>& termDict() const;
    SkipList readTermSkipList(term) const;
};
```

---

### `segment/segment_merger.h` — Segment 合并与软删除

```cpp
enum class MergePolicy { FORCE, TIERED };

struct MergeStats {
    uint32_t input_segment_count, input_doc_count;
    uint32_t deleted_doc_count, output_doc_count, output_segment_id;
    size_t   input_bytes, output_bytes;
};

class SegmentMerger {
    explicit SegmentMerger(dir);

    // 软删除
    bool softDelete(DocId);                      // 在所有 Segment 中搜索并标记
    int  softDeleteBatch(vector<DocId>);
    bool isAlive(DocId) const;

    // 合并
    MergeStats mergeAll(new_segment_id);         // 强制合并所有 Segment
    MergeStats mergeIfNeeded(policy=TIERED, max_segments=5);

    // 注册表
    vector<uint32_t> activeSegmentIds() const;
    void printStats() const;
};
```

---

### `core/index_writer.h` — 文档写入入口

```cpp
class IndexWriter {
    IndexWriter(dir, ram_buffer_mb=16.f, schema=Schema::defaultSchema());
    ~IndexWriter();

    void addDocument(const Document&);
    void deleteDocument(DocId);
    void flush();   // 手动触发（RAM 超阈值自动触发）
    void commit();

    uint32_t totalDocs()    const;
    uint32_t segmentCount() const;
    const vector<SegmentWriteStats>& segStatsHistory() const;
    const vector<FFWriteStats>&      ffStatsHistory()  const;
};
```

---

### `query/index_searcher.h` — 检索入口

```cpp
enum class QueryMode { AND, OR };

class IndexSearcher {
    explicit IndexSearcher(dir);   // 加载所有 Segment

    // 基础搜索（query 空格分隔）
    vector<SearchResult> search(query, top_k=10, mode=AND) const;

    // 带数值过滤（FastField in-filter）
    vector<SearchResult> search(query, NumericFilter, top_k=10, mode=AND) const;

    static void printResults(const vector<SearchResult>&);
    void setDebug(bool);   // 开启 IDF/SkipNode/UB 调试输出
};
```

---

## 当前实现状态

### 核心索引

| 功能 | 状态 | 说明 |
|------|------|------|
| Schema 结构 + JSON 持久化 | ✅ | `fieldTypeToStr/FromStr` 覆盖所有类型 |
| Document 泛化（`map<string, FieldVal>`） | ✅ | `set()`/`getString()`/`getInt64()`/`getFloat()`/`getStrList()` |
| FieldDescriptor 类层级 | ✅ | `FixedField<T>`, `VarSingleField`, `VarMultiField`, `CombinedField` |
| StoredDoc 泛化（`.fdt` 格式改造） | ✅ | 泛化 `map` 格式；`StoredDocResult` 保留向后兼容访问器 |
| FastField 泛化（按 Schema 字段名） | ✅ | 任意字段名列存；`FastFieldReader::getInt64/getFloat32(name, idx)` |
| VarMultiField（`multi:true`） | ✅ | `vector<string>` 多值；`buildDescriptors()` 路由 |
| CombinedField 虚拟字段 | ✅ | `sources` 联合分词；`buildTokensDoc()` 重载 |

### Per-Field 倒排索引（design/per_field_inverted_index.md，Step 1–8 全部完成）

| 功能 | 状态 | 说明 |
|------|------|------|
| Token.field + InMemoryIndex per-field 路由 | ✅ | `field_indexes_[field]`；Token 携带字段名 |
| SegmentWriter per-field 输出 | ✅ | `_N.tim_<field>` / `_N.doc_<field>` / `_N.pos_<field>`（FreqsPositions only）|
| SegmentReader per-field API | ✅ | `getTermMeta(field,term)` / `postingIterator(field,term)` / `bm25Score(field,...)` |
| avg_doc_len 按字段独立统计 | ✅ | `field_avg_doc_lens_` 从 `.tim_<field>` 派生，BM25 per-field 打分 |
| SegmentMerger per-field 合并 | ✅ | 按字段独立多路归并，保留 per-field 文件；`deleteSegmentFiles` 清理 `_N.*_*` |
| IndexSearcher `field:term` 语法 | ✅ | `parseQuery`/`computeFieldTermIdfs`/`scoreDoc` 全部实现 |
| 裸词展开策略 | ✅ | AND：所有默认字段都必须命中；OR：任意字段命中即可 |
| Backward compatibility | ✅ | 旧 Segment（`_N.tim` 无字段后缀）自动 legacy 模式；deprecated API 保留 |
| legacy `readPostingList` per-field 路由修复 | ✅ | 通过 `postingIterator(field,term)` 路由，避免 shared ifstream 状态污染 |
| test_per_field_index 端到端测试 | ✅ | 5 个测试覆盖写入/字段隔离/字段查询/合并/legacy 兼容 |
| wiki_searcher help 说明 `field:term` | ✅ | `usage()` 和文件头注释均已更新 |
| Merge 时 SkipNode.max_score 重算 | ⬜ | `doMerge` 已用最终 N 重算 IDF，但 SkipNode 内 max_score 未同步 |

---

## 待办（TODO）

1. **BlockMaxWAND 真正消费**：`TermMeta::upper_bound` 和 `SkipNode::max_score` 已写入磁盘，但查询时尚未用于 Block 级剪枝（实现后可跳过 90%+ 无效 Block）。
2. **Posting List 惰性迭代器扩展**：`PostingIterator` 已支持 AND，WAND OR 路径仍用全量解压；可扩展为 block-level seek。
3. **Merge 时 SkipNode.max_score 重算**：`SegmentMerger::doMerge()` 已用最终 N 重算 IDF，但 SkipNode.max_score 未同步更新。
4. **stored 字段支持数值类型**：`FixedField<T>::storeStr()` 当前返回空串，需要决策是否在 `.fdt` 存储数值的字符串表示。
5. **字段 Boost**：`scoreDoc` 中各字段权重相等，可在 BM25 层引入 per-field boost 系数（title 命中得分 > body）。
6. **WAND OR 路径迁移为 per-field 惰性迭代器**：当前 `searchOR_WAND` 仍通过 `postingIterator(field, term)` 惰性读取，可进一步实现 Block 级跳过以减少 I/O。

---

## 已知设计权衡

**IDF 的 N（total_docs）问题：** `SegmentWriter` 在 flush 时只能拿到当前 Segment 的文档数，不是最终全局总数。中间 flush 的 UB/IDF 是近似值。`SegmentMerger` 已在合并时用最终 N 重算 IDF，但 SkipNode 内的 `max_score` 尚未同步。

**Posting List 全量解压：** `readPostingList()` 将所有 Block 一次解压到 `vector`，内存 O(N)。`SkipList` 和 `upper_bound` 已写入磁盘，`PostingIterator` 已支持惰性按需解压（AND 路径），WAND OR 路径待迁移。

**BlockMaxWAND 未消费：** `TermMeta::upper_bound` 和 `SkipNode::max_score` 已写入但查询时未用于 Block 级跳过。实现后可跳过 90%+ 无效 Block。

**跨字段位置偏移（field_pos_base）：** `addDocument` 内跨字段累积位置基准，保证同一 doc 内所有字段的 token 位置全局单调递增。`CombinedField` 也参与此偏移链。修改 Schema 字段顺序会影响位置分布，索引需重建。

**StoredDoc 多值字段分隔符：** `VarMultiField::storeStr()` 用 `\x1f`（Unit Separator）拼接多值，读取端需同样分割。当前 `StoredDocResult` 未提供自动分割的 accessor，调用方自行处理。

**legacy `readPostingList(term)` 路由：** per-field 模式下，该接口通过 `postingIterator(field, term)` 内部实现，每次调用创建独立文件句柄。若调用频繁（如统计全量词项）性能略低于直接 per-field API，但避免了 shared `ifstream` 的 seek 状态污染（早期 bug 的根本原因）。

**`term_dict_`（legacy 合并词典）的语义：** per-field 模式下 `term_dict_` 是跨字段 term 的合并视图；对同时出现在多个字段的 term，`posting_offset` 只保留第一个字段（按字段名字母序）的值，`doc_freq` 取最大值，`total_term_freq` 求和。该词典仅供 deprecated API 兼容，新代码请用 `fieldTermDict(field)` 或 `getTermMeta(field, term)`。

---

## 代码风格

**命名：**
- 类/结构体：`PascalCase`（`PostingList`、`SegmentWriter`）
- 方法/函数：`camelCase`（`addDocument`、`calcIdf`）
- 私有成员变量：`snake_case_` 带下划线后缀（`total_docs_`、`ff_writer_`）
- 常量/枚举值：`PascalCase`（`IndexOption::FreqsPositions`）
- 文件名：`snake_case`（`fast_field_writer.cpp`）

**接口设计原则：**
- 头文件只放声明，实现全在 `.cpp`；头文件注释说明"职责边界"和文件格式
- 新接口用 Schema 驱动的泛化版本，旧命名接口（`pubtime(idx)`、`source()`）作为兼容层保留
- 新模块无第三方库，JSON/二进制解析全部手写

**注释风格：**
- 函数内无逐行注释，在必要处说明 WHY（非显而易见的约束、已知权衡、占位符待填）
- 文件顶部用 `// ──────` 分隔区块，标注"格式说明"和"算法步骤"
- `// TODO:` 标注已知缺陷和待优化点

**错误处理：**
- 文件 I/O 失败抛 `std::runtime_error`（不静默失败）
- FastField 缺失文件静默跳过（旧 Segment 兼容性场景）
- 测试用 `FAIL(msg)` + `assert(false)`，不用异常

**测试约定：**
- 每个模块对应 `tests/<module>/test_<module>.cpp`
- 磁盘测试使用 `/tmp/test_<suffix>` 临时目录，测试结束 `fs::remove_all` 清理
- `tmpDir(suffix)` 辅助函数统一管理临时目录（见 `tests/fastfield/test_fast_field.cpp`）
- 新测试模块需同步在 `CMakeLists.txt` 添加 `add_executable` + `target_link_libraries`
