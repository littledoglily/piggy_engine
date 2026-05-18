# piggy_engine

Lucene 风格的 C++ 全文搜索引擎，支持 per-field 倒排索引、BM25、PForDelta 压缩、FastField 列存。

## Build & Run

\```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
\```

## 测试

\```bash
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
./build/demo
\```

测试框架：自研宏（`tests/test_utils.h`），TEST/PASS/FAIL，无第三方依赖。

## 工具

\```bash
./build/wiki_indexer --input <dir> --output ./wiki_index --schema my_schema.json --ram 256
./build/wiki_searcher --index ./wiki_index --query "body:python language" --mode AND --top 5
\```

查询语法：裸词展开到所有字段，`field:term` 限定字段，两者可混用。

## 当前状态

Schema、Document、FieldDescriptor、StoredDoc、FastField、VarMultiField、CombinedField 全部完成。Per-field 倒排索引 Step 1–8 全部完成，含 wiki_searcher `field:term` 语法。

未完成：BlockMaxWAND Block 级剪枝、Merge 时 SkipNode.max_score 重算、stored 字段数值类型、字段 Boost。

## 已知设计权衡

- **IDF 的 N**：flush 时用当前 Segment 文档数（近似），Merge 时重算 IDF，但 SkipNode.max_score 未同步
- **Posting List 全量解压**：AND 路径已用 PostingIterator 惰性解压，WAND OR 路径待迁移
- **StoredDoc 多值分隔符**：`VarMultiField` 用 `\x1f` 拼接，调用方自行分割
- **legacy term_dict_**：per-field 模式下仅供兼容，新代码用 `fieldTermDict(field)`
- **跨字段位置偏移**：修改 Schema 字段顺序会影响位置分布，需重建索引