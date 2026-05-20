# core 模块

## 职责
全局共享的基础类型定义，无任何业务逻辑，零外部依赖。

## 对外接口
`include/core/types.h`：
- `DocId`（uint32_t）、`Pos`（uint32_t）
- `TermMeta`：倒排词典条目，含 posting_offset / skip_offset / tf_data_offset 等
- `PostingEntry`：doc_id + tf + positions
- `IndexOption`：None / FreqsOnly / FreqsPositions
- `FieldType`：Text / Int64 / Float32

## 关键约定
- `TermMeta` 是文件格式的内存映射，字段顺序与 `.tim_<field>` 二进制布局一一对应
- 修改 `TermMeta` 必须同步更新 store/ 的读取逻辑和 index/ 的写入逻辑

## 依赖
无
