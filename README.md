# piggy_engine

Lucene 风格的 C++17 全文搜索引擎，支持实时写入、per-field 倒排索引、BM25 打分、PForDelta 压缩、BlockMaxWAND 剪枝、FastField 列存、并行索引构建。
未来支持向量索引，及同步最新的版本特性

## 架构

```
modules/
  core/       基础类型（DocId / TermMeta / IndexOption），无依赖
  codec/      PForDelta 压缩、SkipList 序列化，纯算法无 IO
  analysis/   文本分析管道（CharFilter → Tokenizer → StopFilter → Stemmer）
  field/      Schema 定义加载、FastField 数值列存读写
  store/      PostingIterator / PosIterator，封装磁盘块级惰性访问
  index/      Segment 写入 / 读取 / 合并，唯一直接操作磁盘倒排文件的层
  query/      QueryParser、Scorer 树、IndexSearcher，只依赖 ISegmentReader 接口
  memory/     实时内存索引（RTIndexThread / FlushWorker / RealtimeIndexManager）
  common/     并发工具（BlockingQueue / ThreadPool / KwayMerge）

tests/        单元测试（自研宏框架，无第三方依赖）
tools/        wiki_indexer / wiki_searcher / bench_search / demo / tim_reader / doc_reader
```

模块依赖图：

```
core ◄── codec ◄── store ◄──┐
core ◄── analysis             ├── index ◄── query
core ◄── field  ◄─────────────┘
                                    ▲
memory ──────────────────────────────┘ (依赖 index + query)
common ◄── memory
```

## 编译

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

Release 编译：`-DCMAKE_BUILD_TYPE=Release`

## 工具

```bash
# 建索引（Wikipedia 转储或任意 JSON 文档集）
./build/wiki_indexer --input <dir> --output ./wiki_index --schema my_schema.json --ram 256

# 搜索
./build/wiki_searcher --index ./wiki_index --query "body:python language" --mode AND --top 5

# 性能基准
./build/bench_search --index ./wiki_index --query "python" --top 10

# 调试工具：查看 .tim_<field> / .doc_<field> 内容
./build/tim_reader
./build/doc_reader

# 简单演示（写 256 篇文档并搜索）
./build/demo
```

查询语法：`+term`（MUST）、`-term`（MUST_NOT）、裸词（SHOULD）、`field:term` 限定字段，可混用。

## 测试

```bash
# 基础模块
./build/test_all
./build/test_analyzer
./build/test_pfor_delta
./build/test_skiplist
./build/test_posting_list
./build/test_posting_iterator
./build/test_merger
./build/test_per_field_index
./build/test_fast_field
./build/test_schema

# Query 层
./build/test_term_scorer
./build/test_conjunction_scorer
./build/test_wand_scorer
./build/test_exclusion_scorer
./build/test_query
./build/test_query_parser
./build/test_index_searcher
./build/test_block_max_wand
./build/test_scorer_tree_e2e

# 并行索引构建
./build/test_parallel_worker_equivalence
./build/test_parallel_no_conflict
./build/test_segment_merge_subset
./build/test_parallel_commit
./build/test_parallel_e2e
./build/test_parallel_merge_fanout

# 实时索引（memory 模块）
./build/test_segment_arena
./build/test_term_hash_table
./build/test_memory_segment_write
./build/test_memory_segment_reader
./build/test_memory_flush
./build/test_realtime_searcher
./build/test_realtime_concurrent
./build/test_flush_queue
./build/test_flush_worker
./build/test_rt_index_thread
./build/test_rt_flush_e2e

# 并发工具
./build/test_blocking_queue
./build/test_thread_pool
./build/test_kway_merge
```

## 功能完成情况

| 功能 | 状态 |
|------|------|
| PForDelta 压缩（Block=128，Delta + 选位宽 + 补丁区） | ✅ |
| SkipList（Block 级两层，序列化到 `.doc_<field>`） | ✅ |
| Per-field 倒排索引（`.tim/.doc/.pos/.len` 各字段独立） | ✅ |
| Segment 写入 / 读取 / 合并（含软删除、IDF 重算） | ✅ |
| 原文存储（`.fdt/.fdx`，按 doc_id seek） | ✅ |
| FastField 列存（数值属性，`.ff_<field>`） | ✅ |
| Schema（JSON 加载，FieldType / IndexOption） | ✅ |
| 文本分析管道（分词 → 小写化 → 词干还原） | ✅ |
| BooleanQuery / TermQuery → Scorer 树 | ✅ |
| BM25 打分（k1=1.2, b=0.75，per-field avgdl） | ✅ |
| ConjunctionScorer（MUST AND，Zigzag 交集） | ✅ |
| WANDScorer（SHOULD OR，UB 剪枝 Top-K） | ✅ |
| ExclusionScorer（MUST_NOT 过滤） | ✅ |
| BlockMaxWAND（SkipNode.max_score + skipBlock 剪枝） | ✅ |
| QueryParser（`+field:term -term bare_term` → BooleanQuery） | ✅ |
| IndexSearcher（跨 Segment 归并，ext_id 去重） | ✅ |
| 并行索引构建（ThreadPool + 分片 flush + kway 扇出合并） | ✅ |
| 实时索引（RTIndexThread + FlushQueue + FlushWorker） | ✅ |
| RealtimeIndexManager（多线程写入，round-robin 分发） | ✅ |
| 实时搜索（MemorySegmentReader + 磁盘 Reader 并发） | ✅ |

## 待完成

- **stored 字段数值类型**：`.fdt` 目前只存字符串；int/float 字段需二进制编码
- **字段 Boost**：`FieldSchema.boost` 已预留，BM25 打分未乘字段权重
- **短语查询**：`.pos_<field>` 已写入位置，缺 `PhraseQuery` / `PhraseScorer`
- **SIMD 解压**：PForDelta 当前为位循环，可替换为 AVX2/AVX512 批量解包
- **deprecated 接口清理**：`IndexSearcher::searchAND` / `searchOR_WAND` 已标 `[[deprecated]]`
