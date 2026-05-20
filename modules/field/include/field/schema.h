#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// field/schema.h  —  索引字段 Schema 定义
// ─────────────────────────────────────────────────────────────────────────────
#include <string>
#include <vector>

namespace ii {

enum class FieldType {
    Text, Keyword, Int64, Float32, Combined
};

enum class IndexOption {
    None, DocsOnly, FreqsOnly, FreqsPositions
};

struct FieldSchema {
    std::string name;
    FieldType   type;
    IndexOption index   = IndexOption::None;
    bool        stored  = false;
    bool        fast    = false;
    bool        multi   = false;
    std::vector<std::string> sources;
};

struct Schema {
    std::vector<FieldSchema> fields;

    const FieldSchema* find(const std::string& name) const;

    std::vector<const FieldSchema*> indexedFields() const;
    std::vector<const FieldSchema*> storedFields()  const;
    std::vector<const FieldSchema*> fastFields()    const;

    void         save(const std::string& index_dir) const;
    static Schema load(const std::string& index_dir);
    static Schema fromJson(const std::string& json_path);
    static Schema defaultSchema();
};

const char* fieldTypeToStr(FieldType t);
const char* indexOptionToStr(IndexOption o);
FieldType   fieldTypeFromStr(const std::string& s);
IndexOption indexOptionFromStr(const std::string& s);

} // namespace ii
