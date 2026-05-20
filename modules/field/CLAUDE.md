# field 模块

## 职责
Schema 定义和加载（字段描述符）；FastField 数值列存读写。
与倒排索引完全解耦，可独立使用。

## 对外接口
- `Schema::load(dir)` → Schema
- `Schema::find(name)` → const FieldSchema*
- `Schema::indexedFields()` / `Schema::fastFields()`
- `FieldSchema`：name / type / index_option / stored / boost
- `FastFieldReader::getInt64(field, doc_idx)` / `getFloat32(field, doc_idx)`
- `FastFieldWriter::addInt64/addFloat32/flush(dir, seg_id)`

## 关键约定
- doc_idx 是 0-indexed（local segment index），与 DocId（1-indexed）差 1
- FastField 文件名：`_N.ff_<field>`
- Schema 从 `<dir>/schema.json` 加载，不存在时使用默认 Schema

## 依赖
core/
