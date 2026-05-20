# 工程模块化拆分设计

## 目标

将当前 ~2W 行的平铺结构拆分为职责清晰的独立模块，使每次 Claude 会话只需加载
2-4 个模块的头文件即可完成开发任务，降低 token 消耗，同时支持各模块独立构建和测试。

---

## 目录结构

```
piggy_engine/
├── modules/
│   ├── core/               # 基础类型，零依赖
│   ├── codec/              # 压缩/解压算法
│   ├── analysis/           # 分词器
│   ├── store/              # Posting 访问层（依赖 codec）
│   ├── field/              # Schema + FastField（依赖 core）
│   ├── index/              # Segment 读写合并（聚合层）
│   └── query/              # 查询/打分层（最顶层）
├── tools/                  # 可执行工具（wiki_indexer, wiki_searcher, bench_search）
├── tests/                  # 集成测试（跨模块）
├── design/                 # 设计文档
└── CMakeLists.txt          # 顶层，include 各模块
```

每个模块内部结构统一：

```
modules/<name>/
├── CLAUDE.md       # 模块职责、接口约定、关键不变量（供 Claude 快速定位）
├── include/        # 对外暴露的头文件（其他模块只能 include 这里）
├── src/            # 实现文件（外部不可见）
├── tests/          # 本模块单元测试
└── CMakeLists.txt  # 独立可构建
```

---

## 模块详情

### 1. core/

**职责**：全局共享的基础类型定义，无任何业务逻辑。

**当前来源**：`include/types.h`

**对外接口（include/core/types.h）**：

```cpp
using DocId = uint32_t;
using Pos   = uint32_t;

struct TermMeta {
    uint32_t doc_freq;
    uint32_t total_term_freq;
    uint64_t posting_offset;
    uint64_t skip_offset;
    uint64_t pos_offset;
    float    upper_bound;
    uint64_t tf_data_offset;
};

struct PostingEntry { DocId doc_id; uint32_t tf; std::vector<Pos> positions; };

enum class IndexOption  { None, FreqsOnly, FreqsPositions };
enum class FieldType    { Text, Int64, Float32 };
```

**依赖**：无

**注意**：`TermMeta` 是文件格式的映射，修改需同步 codec/store/index 三个模块。

---

### 2. codec/

**职责**：纯算法层，负责 DocId 序列的压缩/解压（PForDelta）和跳表序列化（SkipList）。
不含任何文件 IO，不依赖 core/ 以外的任何东西。

**当前来源**：`include/postings/pfor_delta.h`, `include/postings/skiplist.h`

**对外接口**：

```cpp
// PForDelta：输入升序 doc_ids，输出压缩字节流 + SkipNode 列表
class PForDelta {
    static std::vector<uint8_t> compress(const std::vector<DocId>& ids,
                                         std::vector<SkipNode>& skip_out,
                                         const std::vector<float>& block_ubs);
    static std::vector<DocId>   decompress(const std::vector<uint8_t>& data, uint32_t n);
};

// SkipList：两级跳表，支持 advance 加速
class SkipList {
    std::vector<uint8_t> serialize() const;
    static SkipList deserialize(const uint8_t* buf, size_t len);
};
```

**依赖**：core/

**独立测试**：直接构造 `[]uint32` 数据压缩后解压，对比原始序列。无需任何文件。

---

### 3. analysis/

**职责**：文本分析管道（分词 → 归一化 → 过滤），输入字符串，输出 token 列表。

**当前来源**：`include/tokenizer/analyzer.h`, `src/tokenizer/analyzer.cpp`

**对外接口**：

```cpp
class Analyzer {
    std::vector<std::string> analyze(const std::string& text) const;
};
```

**依赖**：无（或 core/ 中 string util）

**独立测试**：给定字符串，断言 token 序列正确。完全不依赖磁盘。

---

### 4. store/

**职责**：封装对 `.doc_<field>` 文件的按需访问，提供 `PostingIterator`。
不知道字段名语义，不知道 BM25，只负责按块解压和迭代。

**当前来源**：`include/postings/posting_iterator.h`, `src/postings/posting_iterator.cpp`,
`include/postings/posting_list.h`, `src/postings/posting_list.cpp`

**对外接口**：

```cpp
class PostingIterator {
    PostingIterator(std::ifstream& doc_file, const TermMeta& meta);
    DocId    docId()         const;
    uint32_t tf()            const;
    bool     next();
    bool     advance(DocId t);
    bool     isEnd()         const;
    float    blockMaxScore() const;   // max_tf_norm，不含 IDF
    DocId    blockMaxDocId() const;
};
```

**文件格式约定（与 index/ 的隐式契约）**：

```
.doc_<field> 布局（每个 term）：
  [SkipList bytes]          ← skip_offset 指向这里
  [PForDelta compressed]    ← posting_offset 指向这里
  [tf bytes: uint8_t × df]  ← tf_data_offset 指向这里
```

> **格式变更视为 breaking change，需同步修改 index/SegmentWriter 和 store/PostingIterator。**

**依赖**：core/, codec/

**独立测试**：由 index/ 的测试辅助函数写出临时 `.doc` 文件，store/ 测试读取它。
或直接在内存中构造合法字节序列（参考 `tests/test_posting_iterator.cpp` 的做法）。

---

### 5. field/

**职责**：Schema 定义和加载（字段描述符）；FastField 列存读写（数值字段）。
与倒排索引完全解耦。

**当前来源**：`include/schema/`, `src/schema/`,
`include/fastfield/`, `src/fastfield/`

**对外接口**：

```cpp
// Schema：从 JSON 加载字段描述，描述每个字段的类型、索引选项
class Schema {
    static Schema load(const std::string& dir);
    const FieldSchema* find(const std::string& name) const;
    std::vector<const FieldSchema*> indexedFields() const;
    std::vector<const FieldSchema*> fastFields()    const;
};

// FastFieldReader/Writer：数值列存，按 doc_idx（0-indexed）随机访问
class FastFieldReader {
    int64_t getInt64  (const std::string& field, uint32_t doc_idx) const;
    float   getFloat32(const std::string& field, uint32_t doc_idx) const;
};
```

**依赖**：core/

**独立测试**：加载测试 JSON schema，断言字段属性；写入数值后读取对比。

---

### 6. index/

**职责**：Segment 的完整生命周期管理——写入（flush）、只读查询（read）、合并（merge）。
聚合所有下层模块，是唯一直接操作磁盘文件的层（除 FastField 外）。

**当前来源**：`include/segment/`, `src/segment/`,
`include/core/index_writer.h`, `src/core/index_writer.cpp`

**对外接口**：

```cpp
// 对 query/ 暴露的抽象接口（隔离 query/ 对具体实现的依赖）
class ISegmentReader {
public:
    virtual PostingIterator postingIterator(const std::string& field,
                                            const std::string& term) const = 0;
    virtual const TermMeta* getTermMeta    (const std::string& field,
                                            const std::string& term) const = 0;
    virtual uint32_t  fieldDocLen    (const std::string& field, DocId doc_id) const = 0;
    virtual float     fieldAvgDocLen (const std::string& field) const = 0;
    virtual uint32_t  docCount()     const = 0;
    virtual uint32_t  segmentId()    const = 0;
    virtual const std::vector<std::string>& indexedFieldNames() const = 0;
    virtual bool      isAlive(DocId doc_id) const = 0;
};

// 具体实现（query/ 通过 ISegmentReader* 使用，不直接 include 此头文件）
class SegmentReader : public ISegmentReader { ... };
class SegmentWriter { ... };
class SegmentMerger { ... };
```

**依赖**：core/, codec/, store/, field/, analysis/

**独立测试**：每个测试在 `/tmp/` 下建临时目录，写入 → 读取 → 断言，测试结束删除。

---

### 7. query/

**职责**：查询解析、Scorer 树构建、跨 Segment 归并排序，输出 TopN 结果。
**只依赖 ISegmentReader 接口，不依赖 SegmentReader 具体实现。**

**当前来源**：`include/query/`, `src/query/`

**对外接口**：

```cpp
// 查询节点
class Query { virtual std::unique_ptr<Scorer> createScorer(const ScorerContext&) const = 0; };
class TermQuery    : public Query { ... };
class BooleanQuery : public Query { ... };   // MUST / SHOULD / MUST_NOT

// Scorer 树
class Scorer      { virtual float score() = 0; virtual bool advance(DocId) = 0; ... };
class TermScorer        : public Scorer { ... };
class ConjunctionScorer : public Scorer { ... };
class WANDScorer        : public Scorer { ... };
class ExclusionScorer   : public Scorer { ... };

// 入口
class IndexSearcher {
    explicit IndexSearcher(const std::string& dir);
    std::vector<SearchResult> search(const Query& q, int top_n) const;
};

// 解析 "+field:term -term bare_term" → BooleanQuery 树
class QueryParser {
    std::unique_ptr<Query> parse(const std::string& expr) const;
};
```

**依赖**：core/, store/, index/（仅 ISegmentReader 接口）

**独立测试**：实现 `MockSegmentReader : public ISegmentReader`，在内存中构造 posting 数据，
不需要任何磁盘文件即可测试全部 Scorer 逻辑。

```cpp
// tests/mock_segment_reader.h（query/ 模块内）
class MockSegmentReader : public ISegmentReader {
    void addPosting(const std::string& field, const std::string& term,
                    std::vector<std::pair<DocId,uint32_t>> docs); // <doc_id, tf>
    // ISegmentReader 接口实现...
};
```

---

## 模块依赖关系总览

```
analysis/ ──────────────────────────────────────┐
                                                 ↓
core/ ──→ codec/ ──→ store/ ──→ index/ ──→ query/
  │                               ↑
  └──────────→ field/ ────────────┘
```

**调用方式示例**：

```
tools/wiki_indexer
  → index/IndexWriter   （写入）
  → analysis/Analyzer   （分词）
  → field/Schema        （字段配置）

tools/wiki_searcher
  → query/QueryParser   （解析查询串）
  → query/IndexSearcher （执行搜索）
    → index/ISegmentReader （per-segment 迭代）
      → store/PostingIterator（惰性解压）
        → codec/PForDelta    （块解压）
```

---

## 拆分步骤（按风险从低到高）

### Step 1：建立目录骨架（无代码移动，零风险）

```bash
mkdir -p modules/{core,codec,analysis,store,field,index,query}/{include,src,tests}
```

每个目录创建占位 `CLAUDE.md`，写明模块职责和接口约定（参考上文各模块描述）。

### Step 2：迁移 core/（零风险）

- 移动 `include/types.h` → `modules/core/include/core/types.h`
- 更新所有 `#include "types.h"` 为 `#include "core/types.h"`
- 建独立 `modules/core/CMakeLists.txt`：`add_library(piggy_core INTERFACE)`
- **验证**：`cmake --build` 全量编译通过

### Step 3：迁移 codec/（零风险）

- 移动 `include/postings/pfor_delta.h` + `.cpp` → `modules/codec/`
- 移动 `include/postings/skiplist.h` + `.cpp` → `modules/codec/`
- 建 `modules/codec/CMakeLists.txt`：`target_link_libraries(piggy_codec piggy_core)`
- 迁移 `tests/test_pfor_delta.cpp`、`tests/test_skiplist.cpp` → `modules/codec/tests/`
- **验证**：`./build/test_pfor_delta && ./build/test_skiplist`

### Step 4：迁移 analysis/（零风险）

- 移动 `include/tokenizer/` + `src/tokenizer/` → `modules/analysis/`
- **验证**：`./build/test_analyzer`

### Step 5：迁移 field/（低风险）

- 移动 `include/schema/` + `src/schema/` → `modules/field/`
- 移动 `include/fastfield/` + `src/fastfield/` → `modules/field/`
- **验证**：`./build/test_schema && ./build/test_fast_field`

### Step 6：迁移 store/（低风险，依赖 codec）

- 移动 `include/postings/posting_iterator.h` + `.cpp` → `modules/store/`
- 移动 `include/postings/posting_list.h` + `.cpp` → `modules/store/`
- 在 `modules/store/CLAUDE.md` 中明确记录 `.doc_<field>` 文件格式
- **验证**：`./build/test_posting_iterator && ./build/test_posting_list`

### Step 7：迁移 index/ + 提取 ISegmentReader（中风险）

- 移动 `include/segment/` + `src/segment/` → `modules/index/`
- 移动 `include/core/index_writer.h` + `src/core/index_writer.cpp` → `modules/index/`
- 新增 `modules/index/include/index/i_segment_reader.h`（纯虚接口）
- `SegmentReader` 继承 `ISegmentReader`
- **验证**：`./build/test_merger && ./build/test_per_field_index`

### Step 8：迁移 query/（中风险）

- 移动 `include/query/` + `src/query/` → `modules/query/`
- `ScorerContext` 中 `const SegmentReader& seg` 改为 `const ISegmentReader& seg`
- 新增 `modules/query/tests/mock_segment_reader.h`
- **验证**：全部 query 层测试通过

### Step 9：更新 tools/ 和顶层 CMakeLists.txt

- `tools/wiki_indexer.cpp` 等只改 include 路径
- 顶层 CMakeLists 用 `add_subdirectory(modules/xxx)` 替换原有路径

---

## 关键耦合点备忘

| 耦合点 | 涉及模块 | 管理方式 |
|--------|---------|---------|
| `.doc_<field>` 文件格式 | store/ ↔ index/ | `store/CLAUDE.md` 写明格式规范，格式变更为 breaking change |
| `TermMeta` 结构 | core/ → store/ → index/ | 加字段需同步三个模块；长期可考虑将 TermMeta 移入 index/ |
| `ISegmentReader` 接口 | index/ → query/ | 接口变更需同步 MockSegmentReader |

---

## 各模块独立测试命令（拆分后）

```bash
cmake --build build --target piggy_core   && ./build/modules/core/test_core
cmake --build build --target piggy_codec  && ./build/modules/codec/test_codec
cmake --build build --target piggy_store  && ./build/modules/store/test_store
cmake --build build --target piggy_field  && ./build/modules/field/test_field
cmake --build build --target piggy_index  && ./build/modules/index/test_index
cmake --build build --target piggy_query  && ./build/modules/query/test_query
```
