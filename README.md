# piggy_engine

Lucene 风格的 C++17 全文搜索引擎。

## 已完成模块

| 模块 | 说明 |
|------|------|
| `analyzer` | CharFilter → Tokenizer → StopFilter → Stemmer 分析管道 |
| `pfor_delta` | Delta + 选位宽 + 补丁区，Block 大小 128 |
| `skiplist` | Block 级两层跳表，序列化到 `.doc` |
| `posting_list` / `posting_iterator` | 内存倒排链 + 惰性解压迭代器 |
| `segment_writer/reader/merger` | Flush → `.tim/.doc/.pos/.fdt/.fdx/.liv/.si`，含软删除与合并 |
| `index_writer` | RAM Buffer + 超阈值自动 flush |
| `schema` / `field_descriptor` | Per-field 倒排索引，多字段类型 |
| `fast_field` | 列存（数值属性字段） |
| **Query 层** | `TermQuery` / `BooleanQuery`(MUST/SHOULD/MUST_NOT) → Scorer 树 |
| `term_scorer` | BM25 打分 + PostingIterator 惰性迭代 |
| `conjunction_scorer` | MUST AND，Zigzag 交集 |
| `wand_scorer` | SHOULD OR，WAND UB 剪枝 Top-K |
| `exclusion_scorer` | MUST_NOT 过滤 |
| `query_parser` | `+field:term -term bare_term` → BooleanQuery 树 |
| `index_searcher` | 跨 Segment 归并，调用 Query/Scorer 树 |

## 工程结构

```
piggy_engine/
├── include/          头文件（按模块分目录）
│   ├── types.h
│   ├── core/         index_writer
│   ├── postings/     pfor_delta, skiplist, posting_list, posting_iterator
│   ├── segment/      segment_writer/reader/merger
│   ├── tokenizer/    analyzer
│   ├── schema/       schema, field_descriptor
│   ├── fastfield/    fast_field_writer/reader
│   └── query/        query, query_parser, scorer 树
├── src/              实现文件（与 include/ 同结构）
├── tests/            各模块单元测试
└── tools/            wiki_indexer, wiki_searcher, bench_search
```

## 编译运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4

./build/demo          # 写入 256 篇文档 + 搜索
```

## 测试

```bash
# 各模块测试（选择性运行）
./build/test_all
./build/test_analyzer
./build/test_pfor_delta
./build/test_skiplist
./build/test_posting_list
./build/test_posting_iterator
./build/test_merger
./build/test_fast_field
./build/test_schema
./build/test_per_field_index

# Query 层测试
./build/test_term_scorer
./build/test_conjunction_scorer
./build/test_wand_scorer
./build/test_exclusion_scorer
./build/test_query
./build/test_query_parser
./build/test_index_searcher
./build/test_block_max_wand
./build/test_scorer_tree_e2e
```

## 工具

```bash
# 建索引
./build/wiki_indexer --input <dir> --output ./wiki_index --schema my_schema.json --ram 256

# 搜索（支持 field:term 和裸词展开）
./build/wiki_searcher --index ./wiki_index --query "body:python language" --mode AND --top 5

# 性能基准
./build/bench_search --index ./wiki_index --query "python" --top 10
```

查询语法：`+term`（MUST）、`-term`（MUST_NOT）、裸词（SHOULD）、`field:term` 限定字段，可混用。

## 未完成 / 已知缺口

- **BlockMaxWAND**：`SkipNode.max_score` 已写入文件，查询侧 Block 级剪枝尚未接入
- **Merge 时 max_score 重算**：Segment 合并后 SkipNode 上界未同步
- **stored 字段数值类型** / **字段 Boost**
- **SIMD 解压**：当前为位循环，未使用 AVX2/AVX512
