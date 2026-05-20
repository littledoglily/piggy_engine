# query 模块

## 职责
查询解析、Scorer 树构建执行、跨 Segment TopN 归并。
只依赖 ISegmentReader 接口，不依赖 SegmentReader 具体实现。

## 对外接口
- `QueryParser::parse(expr)` → unique_ptr<Query>
  - 语法：`+field:term`（MUST）、`-term`（MUST_NOT）、裸词（SHOULD）
- `IndexSearcher::search(query, top_n)` → vector<SearchResult>
- `BooleanQuery` / `TermQuery` → Query 基类
- Scorer 树：`TermScorer` / `ConjunctionScorer` / `WANDScorer` / `ExclusionScorer`

## BM25 打分
`TermScorer::score()` 使用完整 BM25（k1=1.2, b=0.75）：
```
tf * (k1+1) / (tf + k1*(1-b + b*dl/avgdl)) * idf
```
- tf、dl 通过 `PostingIterator::tf()` 和 `ISegmentReader::fieldDocLen()` 获取
- idf 由 IndexSearcher 跨 Segment 预算后存入 ScorerContext

## 独立测试
tests/mock_segment_reader.h 提供内存 mock，无需任何磁盘文件。

## 依赖
core/, store/, index/（仅 ISegmentReader 接口）
