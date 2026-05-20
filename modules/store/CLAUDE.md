# store 模块

## 职责
封装对 `.doc_<field>` 文件的惰性按块访问，提供 `PostingIterator`。
不感知字段名语义，不感知 BM25，只负责块解压和迭代。

## 对外接口
- `PostingIterator(ifstream& doc_file, const TermMeta& meta)`
- `docId()` / `tf()` / `next()` / `advance(DocId)` / `isEnd()`
- `blockMaxScore()`：当前块的 max_tf_norm（不含 IDF，由上层 TermScorer 乘入）
- `blockMaxDocId()`：当前块的最大 DocId（用于 BlockMaxWAND skipBlock）

## .doc_<field> 文件格式（与 index/ 的隐式契约）
每个 term 的数据布局：
```
[SkipList bytes]          ← TermMeta.skip_offset 指向这里
[PForDelta compressed]    ← TermMeta.posting_offset 指向这里
[tf bytes: uint8_t × df]  ← TermMeta.tf_data_offset 指向这里
```
**格式变更为 breaking change，必须同步修改 index/SegmentWriter。**

## 依赖
core/, codec/
