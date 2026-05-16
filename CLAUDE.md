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
./build/test_all           # 集成测试（Analyzer + PForDelta + Merger + Searcher）
./build/test_analyzer
./build/test_pfor_delta
./build/test_skiplist
./build/test_posting_list
./build/test_merger
./build/test_fast_field
./build/test_schema

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

# 单次查询
./build/wiki_searcher --index ./wiki_index --query "machine learning" --mode AND --top 5
```

`wiki_indexer` Schema 加载优先级：`--schema 文件` → 输出目录中的 `schema.json`（续建） → `defaultSchema()`。

---

## 架构概览

**核心数据流：**

```
Document
  └─ IndexWriter.addDocument()
       ├─ Schema 路由每个字段
       │   ├─ Text/Keyword 字段 → Analyzer → InMemoryIndex（倒排）
       │   ├─ fast=true 字段   → FastFieldWriter（列存缓冲）
       │   └─ stored=true 字段 → stored_docs_buf_（.fdt 缓冲）
       └─ RAM 超阈值 → flush() → SegmentWriter + FastFieldWriter::flush()

查询：
IndexSearcher
  └─ 对所有 Segment 并行执行 searchAND/searchOR_WAND
       └─ mergeTopK 跨 Segment 归并
```

**模块职责：**

| 模块 | 关键文件 | 职责边界 |
|------|---------|---------|
| `schema` | `schema.h/cpp` | Schema 定义、JSON 持久化、字段筛选 |
| `tokenizer` | `analyzer.h/cpp` | CharFilter → Tokenizer → StopFilter → Porter Stemmer |
| `postings` | `pfor_delta`, `skiplist`, `posting_list` | 内存倒排链、Block 压缩、跳表 |
| `segment` | `segment_writer/reader/merger` | Flush 文件、只读打开、合并 + 软删除 |
| `fastfield` | `fast_field_writer/reader` | 数值列存（`_N.ff_<field>`），O(1) 随机访问 |
| `core` | `index_writer` | RAM Buffer 管理、Schema 路由、触发 flush |
| `query` | `index_searcher` | Zigzag AND、WAND OR、跨 Segment 归并、数值过滤 |

**Segment 文件集（每个 Segment 前缀 `_N`）：**

```
_N.tim   Term 词典（全量加载内存）：term → df/ttf/UB/offset
_N.doc   Posting List：SkipList 序列化 + PForDelta Block
_N.pos   位置信息：(doc_id, tf, positions[])
_N.fdt   文档原文：stored 字段定长串联
_N.fdx   文档索引：doc_id → fdt 字节偏移
_N.liv   存活位图：软删除标记
_N.si    Segment 元数据
_N.ff_<field>  FastField 列存：定长二进制数组（int64×8B 或 float×4B）
schema.json    索引级 Schema，存于索引根目录
```

---

## Schema 系统

Schema 在 `IndexWriter` 构建时传入，之后不可变，持久化为 `schema.json`。

```cpp
Schema schema = Schema::fromJson("my_schema.json");  // 或 Schema::defaultSchema()
IndexWriter writer(dir, ram_mb, schema);
```

字段属性正交组合：

| 属性 | 效果 |
|------|------|
| `index: freqs_positions` | 建倒排 + 位置（短语查询） |
| `index: freqs_only` | 建倒排，仅 tf（BM25 无位置）|
| `index: docs_only` | 仅 doc_id（布尔 / 等值过滤）|
| `stored: true` | 写入 `.fdt`，可从搜索结果取回 |
| `fast: true` | 写 `.ff_<name>`，支持 O(1) 数值过滤 / 排序 |

**当前实现状态（设计文档 Step 1-7 中）：**
- ✅ Step 1/3/5：Schema 结构、IndexWriter 路由、FastField 泛化
- ⬜ Step 4：term 字段前缀（`title:python`），avg_doc_len 按字段独立统计
- ⬜ Step 6：Document 泛化为 `map<string, FieldValue>`（当前仍是具名结构体，通过桥接函数映射）
- ⬜ Step 7：IndexSearcher 支持 `field:query` 语法

---

## 已知设计权衡

**IDF 的 N（total_docs）问题：** `SegmentWriter::calcIdf()` 在每次 flush 时只能拿到当前 Segment 的文档数，不是最终全局总数。中间 flush 的 UB/IDF 是近似值。正确做法是 merge 时以最终 N 重算，当前 `SegmentMerger` 尚未实现这一步。

**Posting List 全量解压：** `readPostingList()` 将所有 Block 一次解压到 `vector`，内存 O(N)。`SkipList` 和 `upper_bound` 已写入磁盘，待实现惰性迭代器（Tantivy `BlockSegmentPostings` 模式）后可降为 O(1)。

**BlockMaxWAND 未消费：** `TermMeta::upper_bound` 和 `SkipNode::max_score` 已写入但查询时未用于 Block 级跳过。实现后可跳过 90%+ 无效 Block。

**跨字段位置偏移（field_pos_base）：** `addDocument` 内跨字段累积位置基准，保证同一 doc 内所有字段的 token 位置全局单调递增。这是 Schema 驱动多字段索引的必要约束，修改 Schema 字段顺序会影响位置分布。

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
- 新接口用 Schema 驱动的泛化版本，旧命名接口（`pubtime(idx)`、`add(FastFieldDoc)`）作为兼容层保留，注释标注 `向后兼容`
- 新模块无第三方库，JSON/二进制解析全部手写

**注释风格：**
- 函数内无逐行注释，在必要处说明 WHY（非显而易见的约束、已知权衡、占位符待填）
- 文件顶部用 `// ──────` 分隔区块，标注"格式说明"和"算法步骤"
- `// TODO:` 标注已知缺陷和待优化点（不是 FIXME，不是 HACK）

**错误处理：**
- 文件 I/O 失败抛 `std::runtime_error`（不静默失败）
- FastField 缺失文件静默跳过（旧 Segment 兼容性场景）
- 测试用 `FAIL(msg)` + `assert(false)`，不用异常

**测试约定：**
- 每个模块对应 `tests/<module>/test_<module>.cpp`
- 磁盘测试使用 `/tmp/test_<suffix>` 临时目录，测试结束 `fs::remove_all` 清理
- `tmpDir(suffix)` 辅助函数统一管理临时目录（见 `tests/fastfield/test_fast_field.cpp`）
- 新测试模块需同步在 `CMakeLists.txt` 添加 `add_executable` + `target_link_libraries`
