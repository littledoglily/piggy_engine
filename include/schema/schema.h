#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// schema.h  —  索引字段 Schema 定义
//
// 设计原则：
//   - Schema 在 Index 创建时确定，之后不可变（immutable）
//   - 持久化为 schema.json，保存在索引目录根部
//   - 读取时若 schema.json 不存在，回退到 defaultSchema()（向后兼容）
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <vector>

namespace ii {

// ── 字段数据类型 ──────────────────────────────────────────────────────────────
enum class FieldType {
    Text,     // 经过 Analyzer 分词；支持 index / stored
    Keyword,  // 不分词，整体作为单个 term；支持 index / stored
    Int64,    // 64-bit 整数；支持 fast / stored
    Float32,  // 32-bit 浮点；支持 fast / stored
    Combined  // 虚拟字段：联合多个源字段分词建倒排，不 stored / fast
};

// ── 倒排索引精度（对标 Lucene IndexOptions）───────────────────────────────────
// 可扩展：BKD tree 是 FastField 层的优化，不是 IndexOption，此处无需添加
enum class IndexOption {
    None,            // 不建倒排
    DocsOnly,        // 只存 doc_id（布尔检索 / 等值过滤）
    FreqsOnly,       // doc_id + tf（BM25 无位置）
    FreqsPositions   // doc_id + tf + 位置（支持短语查询）
};

// ── 单字段完整描述 ─────────────────────────────────────────────────────────────
struct FieldSchema {
    std::string name;
    FieldType   type;
    IndexOption index   = IndexOption::None;
    bool        stored  = false;   // true → 写入 .fdt，可从结果取回
    bool        fast    = false;   // true → 写入 .ff_<name>，支持过滤 / 排序
    bool        multi   = false;   // true → 字段值为 vector<string>（多值）
    std::vector<std::string> sources;  // Combined 字段的源字段列表
};

// ── Index 级别 Schema（不可变）────────────────────────────────────────────────
struct Schema {
    std::vector<FieldSchema> fields;

    // 按名称查找字段（未找到返回 nullptr）
    const FieldSchema* find(const std::string& name) const;

    // 按条件筛选字段列表（IndexWriter 路由用）
    std::vector<const FieldSchema*> indexedFields() const;  // index != None
    std::vector<const FieldSchema*> storedFields()  const;  // stored == true
    std::vector<const FieldSchema*> fastFields()    const;  // fast == true

    // 持久化 / 加载（index_dir 为索引目录路径）
    void         save(const std::string& index_dir) const;
    static Schema load(const std::string& index_dir);   // 不存在则返回 defaultSchema()
    static Schema fromJson(const std::string& json_path);

    // 与当前硬编码行为等价的默认 Schema（向后兼容）
    static Schema defaultSchema();
};

// ── 字符串转换辅助 ────────────────────────────────────────────────────────────
const char* fieldTypeToStr(FieldType t);
const char* indexOptionToStr(IndexOption o);
FieldType   fieldTypeFromStr(const std::string& s);
IndexOption indexOptionFromStr(const std::string& s);

} // namespace ii
