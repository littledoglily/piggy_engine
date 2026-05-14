# 2026.05.14 裸版本实现，实现index_merge, index_build, index_search 接口
- 更好的接口和功能抽象
- 借鉴开源优秀代码打造性能更好的实现，比如Tantivy BlockSegmentPostings → 补你的 readPostingList; Pisa block_max_wand_query → 补你的多 term AND/OR 查询
- 复杂归并树指导and, or, not检索
- dsl语言指定查询语法，类似sql

// 当前：一次性把所有 doc_id 解压到 vector
std::vector<DocId> readPostingList(const std::string& term) const;
Tantivy 的 BlockSegmentPostings 是迭代器模式，每次只解压一个 128-doc Block，调用方 advance(target) 时才推进：


你现在：                         Tantivy：
读 term → 解压全部 N 个 block   读 term → 只解压第 1 个 block
放到 vector → 遍历              调用 advance() → 按需解压下一个 block
内存: O(N)                      内存: O(1)，始终只有 1 个 block
你的 SkipList 已经在 .doc 里，readPostingListFrom 也已经用了它，但仍然是批量返回 vector。Tantivy 告诉你怎么把 SkipList + Block 做成真正的惰性迭代器。

Pisa block_max_wand_query → 补你的多 term AND/OR 查询
你的 upper_bound 字段已经写进 .tim（segment_writer.cpp:169），但从未被用于剪枝。现在的多 term 查询是暴力求交/并集。

Pisa 的 BlockMaxWAND 做的事：


你现在：
  对每个 doc_id，对所有 query terms 算 BM25 → 加总 → 放堆

BlockMaxWAND：
  维护一个 pivot term（按 upper_bound 排序）
  如果 pivot 之前所有 term 的 upper_bound 之和 < 堆的最小分
  → 整个 block 跳过，不解压，不打分
  → 理论上可以跳过 90%+ 的 block
你的 SkipNode.max_score 和 TermMeta.upper_bound 就是 BlockMaxWAND 需要的两个剪枝信号，现在都有了，但没有查询层来消费它们
一句话总结
缺口	现状	参考后能补上的
Tantivy BlockSegmentPostings	全量解压，内存 O(N)	惰性迭代，内存 O(1)，多 term 交集更快
Pisa BlockMaxWAND	upper_bound 写了没用	Top-K 查询跳过 90%+ 无效 block
两者加起来，就是 Lucene 里 DISI（DocIdSetIterator）+ WANDScorer 的完整链路——你的数据结构已经齐了，缺的是把它们串起来的查询执行层。
