# codec 模块

## 职责
纯算法层：DocId 序列的 PForDelta 压缩/解压，SkipList 序列化/反序列化。
不含任何文件 IO，不感知字段语义。

## 对外接口
- `PForDelta::compress(ids, skip_out, block_ubs)` → 压缩字节流 + SkipNode 列表
- `PForDelta::decompress(data, n)` → 原始 DocId 序列
- `SkipList::serialize()` / `SkipList::deserialize(buf, len)`
- `SkipNode`：block 级跳表节点，含 max_score（max_tf_norm，不含 IDF）

## 关键约定
- BLOCK_SIZE = 128（压缩块大小）
- 输入 doc_ids 必须严格升序
- block_ubs 与压缩块一一对应，长度 = ceil(n / BLOCK_SIZE)
- max_score 存 tf_norm（不乘 IDF），IDF 由上层 TermScorer 在运行时乘入

## 依赖
core/
