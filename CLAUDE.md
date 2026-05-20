# piggy_engine

Lucene 风格的 C++ 全文搜索引擎，支持 per-field 倒排索引、BM25、PForDelta 压缩、FastField 列存。

## Build & Run

\```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
\```

## 测试

\```bash
# 基础模块
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

# Query 层（Step 7–9）
./build/test_term_scorer
./build/test_conjunction_scorer
./build/test_wand_scorer
./build/test_exclusion_scorer
./build/test_query
./build/test_query_parser
./build/test_index_searcher
./build/test_block_max_wand
./build/test_scorer_tree_e2e

./build/demo
\```

测试框架：自研宏（`tests/test_utils.h`），TEST/PASS/FAIL，无第三方依赖。

## 工具

\```bash
./build/wiki_indexer --input <dir> --output ./wiki_index --schema my_schema.json --ram 256
./build/wiki_searcher --index ./wiki_index --query "body:python language" --mode AND --top 5
./build/bench_search  --index ./wiki_index --query "python" --top 10
\```

查询语法：`+term`（MUST）、`-term`（MUST_NOT）、裸词（SHOULD），`field:term` 限定字段，可混用。

## 当前状态

所有核心模块已完成：

| 阶段 | 内容 | 状态 |
|------|------|------|
| 基础索引 | PForDelta、SkipList、PostingList、PostingIterator | ✅ |
| Segment | Writer/Reader/Merger，含软删除与 IDF/UB Merge 时重算 | ✅ |
| 存储层 | StoredDoc（fdt/fdx）、FastField 列存、Schema/FieldDescriptor | ✅ |
| Per-field 倒排 | Step 1–8，含 wiki_searcher `field:term` 语法 | ✅ |
| Query 层 | TermQuery/BooleanQuery(MUST/SHOULD/MUST_NOT) → Scorer 树 | ✅ |
| Scorer 树 | TermScorer、ConjunctionScorer、WANDScorer、ExclusionScorer | ✅ |
| BlockMaxWAND | `skipBlock()` + `use_block_max` + `blocks_skipped` 统计 | ✅ |
| QueryParser | `+field:term -term bare_term` → BooleanQuery 树 | ✅ |
| IndexSearcher | BooleanQuery 驱动，跨 Segment 归并 | ✅ |

## TODO

优先级从高到低：

1. **清理 deprecated 接口**：`IndexSearcher::searchAND` / `searchOR_WAND` 已标 `[[deprecated]]`，可在确认无调用后删除。

2. **stored 字段数值类型**：`.fdt` 目前只存字符串；数值字段（int/float）需要二进制编码+按类型读取。

3. **字段 Boost**：`FieldDescriptor` 预留了 `boost` 字段但 BM25 打分未消费，需在 `TermScorer::score()` 中乘以字段权重。

4. **短语查询**：`.pos` 文件已写入位置信息，缺 `PhraseQuery` 节点和 `PhraseScorer`（需位置列表对齐）。

5. **SIMD 解压**：PForDelta 当前是位循环，可替换为 AVX2/AVX512 批量解包。

## 已知设计权衡

- **IDF 的 N**：flush 时用当前 Segment 文档数（近似），Merge 时用合并后总文档数重算 IDF 和 SkipNode.max_score
- **WAND OR 路径迭代**：WANDScorer 的子节点是 TermScorer，已通过 PostingIterator 惰性解压
- **StoredDoc 多值分隔符**：`VarMultiField` 用 `\x1f` 拼接，调用方自行分割
- **legacy term_dict_**：per-field 模式下仅供兼容，新代码用 `fieldTermDict(field)`
- **跨字段位置偏移**：修改 Schema 字段顺序会影响位置分布，需重建索引
- **旧搜索接口**：`IndexSearcher` 保留了 `deprecated` 的 `searchAND/searchOR_WAND`，实际查询已全部走 BooleanQuery/Scorer 树