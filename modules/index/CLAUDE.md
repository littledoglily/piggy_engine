# index 模块

## 职责
Segment 的完整生命周期：写入（flush）、只读查询（read）、合并（merge）。
唯一直接操作磁盘倒排文件的层，聚合 codec/store/field/analysis 各模块。

## 对外接口（对 query/ 暴露）
```cpp
class ISegmentReader {
    virtual PostingIterator postingIterator(field, term) const = 0;
    virtual const TermMeta* getTermMeta(field, term)    const = 0;
    virtual uint32_t  fieldDocLen(field, doc_id)        const = 0;
    virtual float     fieldAvgDocLen(field)             const = 0;
    virtual uint32_t  docCount()                        const = 0;
    virtual uint32_t  segmentId()                       const = 0;
    virtual const vector<string>& indexedFieldNames()   const = 0;
    virtual bool      isAlive(DocId)                    const = 0;
};
```
query/ 只依赖 `ISegmentReader`，不直接 include `SegmentReader`。

## 文件格式
| 文件 | 内容 | 加载方式 |
|------|------|---------|
| `_N.tim_<field>` | term 词典 | 启动时全量加载进 map |
| `_N.doc_<field>` | 压缩 posting | 惰性按块读取 |
| `_N.pos_<field>` | 位置信息 | 按需读取 |
| `_N.len_<field>` | per-doc 字段长度 | 启动时全量加载 |
| `_N.fdt/fdx` | 原文存储 | 按 doc_id seek |
| `_N.liv` | 软删除位图 | 启动时全量加载 |
| `_N.si` | Segment 元信息 | 启动时读取 |

## 依赖
core/, codec/, store/, field/, analysis/
