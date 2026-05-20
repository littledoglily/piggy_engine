# analysis 模块

## 职责
文本分析管道：输入原始字符串，输出 token 列表（分词 → 小写归一化 → 过滤）。
与索引、查询、存储完全解耦。

## 对外接口
- `Analyzer::analyze(text)` → `vector<string>`

## 关键约定
- 当前实现：空格分词 + 小写化，无停用词过滤
- 索引写入和查询解析使用同一个 Analyzer 实例，保证一致性

## 依赖
无（或 core/ 中 string util）
