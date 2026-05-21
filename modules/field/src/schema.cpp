#include "field/schema.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <filesystem>

namespace ii {

// ── 字符串转换辅助 ─────────────────────────────────────────────────────────────

const char* fieldTypeToStr(FieldType t) {
    switch (t) {
        case FieldType::Text:     return "text";
        case FieldType::Keyword:  return "keyword";
        case FieldType::Int64:    return "int64";
        case FieldType::Float32:  return "float32";
        case FieldType::Combined: return "combined";
    }
    return "text";
}

const char* indexOptionToStr(IndexOption o) {
    switch (o) {
        case IndexOption::None:           return "none";
        case IndexOption::DocsOnly:       return "docs_only";
        case IndexOption::FreqsOnly:      return "freqs_only";
        case IndexOption::FreqsPositions: return "freqs_positions";
    }
    return "none";
}

FieldType fieldTypeFromStr(const std::string& s) {
    if (s == "keyword")  return FieldType::Keyword;
    if (s == "int64")    return FieldType::Int64;
    if (s == "float32")  return FieldType::Float32;
    if (s == "combined") return FieldType::Combined;
    return FieldType::Text;
}

IndexOption indexOptionFromStr(const std::string& s) {
    if (s == "docs_only")       return IndexOption::DocsOnly;
    if (s == "freqs_only")      return IndexOption::FreqsOnly;
    if (s == "freqs_positions") return IndexOption::FreqsPositions;
    return IndexOption::None;
}

// ── Schema 查询方法 ───────────────────────────────────────────────────────────

const FieldSchema* Schema::find(const std::string& name) const {
    for (const auto& f : fields) {
        if (f.name == name) return &f;
    }
    return nullptr;
}

std::vector<const FieldSchema*> Schema::indexedFields() const {
    std::vector<const FieldSchema*> result;
    for (const auto& f : fields) {
        if (f.index != IndexOption::None) result.push_back(&f);
    }
    return result;
}

std::vector<const FieldSchema*> Schema::storedFields() const {
    std::vector<const FieldSchema*> result;
    for (const auto& f : fields) {
        if (f.stored) result.push_back(&f);
    }
    return result;
}

std::vector<const FieldSchema*> Schema::fastFields() const {
    std::vector<const FieldSchema*> result;
    for (const auto& f : fields) {
        if (f.fast) result.push_back(&f);
    }
    return result;
}

// ── JSON 序列化 ───────────────────────────────────────────────────────────────

void Schema::save(const std::string& index_dir) const {
    std::string path = index_dir + "/schema.json";
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("Cannot write schema.json: " + path);

    f << "{\n  \"fields\": [\n";
    for (size_t i = 0; i < fields.size(); ++i) {
        const auto& fs = fields[i];
        f << "    {\n";
        f << "      \"name\": \""   << fs.name                    << "\",\n";
        f << "      \"type\": \""   << fieldTypeToStr(fs.type)    << "\",\n";
        f << "      \"index\": \""  << indexOptionToStr(fs.index) << "\",\n";
        f << "      \"stored\": "   << (fs.stored ? "true" : "false") << ",\n";
        f << "      \"fast\": "     << (fs.fast   ? "true" : "false");
        if (fs.multi) {
            f << ",\n      \"multi\": true";
        }
        if (!fs.sources.empty()) {
            f << ",\n      \"sources\": [";
            for (size_t j = 0; j < fs.sources.size(); ++j) {
                if (j > 0) f << ",";
                f << "\"" << fs.sources[j] << "\"";
            }
            f << "]";
        }
        f << "\n    }";
        if (i + 1 < fields.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n}\n";
}

// ── JSON 解析（手写，不依赖第三方库）─────────────────────────────────────────
//
// 格式固定为 Schema::save() 输出的格式：
//   外层 { ... }，fields 数组，每个元素 5 个 k/v 对。
// 解析策略：brace-depth 追踪，depth=2 内是单个 field 对象。

static std::string trimQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

// 从 "key": value 或 "key": "value" 中提取 key 和 value 字符串
static bool parseKV(const std::string& line, std::string& key, std::string& val) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;

    // key 部分
    std::string k = line.substr(0, colon);
    auto l = k.find_first_not_of(" \t");
    auto r = k.find_last_not_of(" \t");
    if (l == std::string::npos) return false;
    key = trimQuotes(k.substr(l, r - l + 1));

    // value 部分（去掉末尾逗号）
    std::string v = line.substr(colon + 1);
    l = v.find_first_not_of(" \t");
    r = v.find_last_not_of(" \t,");
    if (l == std::string::npos) return false;
    val = trimQuotes(v.substr(l, r - l + 1));
    return true;
}

// 从 "sources": ["a","b"] 的值部分（去掉键名和冒号后）提取字符串列表
static std::vector<std::string> parseSources(const std::string& raw) {
    std::vector<std::string> result;
    size_t i = raw.find('[');
    if (i == std::string::npos) return result;
    size_t j = raw.find(']', i);
    if (j == std::string::npos) return result;
    std::string arr = raw.substr(i + 1, j - i - 1);
    size_t pos = 0;
    while (pos < arr.size()) {
        size_t q1 = arr.find('"', pos);
        if (q1 == std::string::npos) break;
        size_t q2 = arr.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        pos = q2 + 1;
    }
    return result;
}

Schema Schema::fromJson(const std::string& json_path) {
    std::ifstream f(json_path);
    if (!f) throw std::runtime_error("Cannot open schema.json: " + json_path);

    Schema schema;
    int depth = 0;
    FieldSchema current;
    bool in_field = false;

    std::string line;
    while (std::getline(f, line)) {
        // 统计 '{' 和 '}' 改变 depth
        for (char c : line) {
            if (c == '{') ++depth;
            else if (c == '}') {
                if (depth == 2 && in_field) {
                    schema.fields.push_back(current);
                    current = FieldSchema{};
                    in_field = false;
                }
                --depth;
            }
        }

        if (depth == 2) {
            in_field = true;
            std::string key, val;
            if (!parseKV(line, key, val)) continue;

            if      (key == "name")    current.name    = val;
            else if (key == "type")    current.type    = fieldTypeFromStr(val);
            else if (key == "index")   current.index   = indexOptionFromStr(val);
            else if (key == "stored")  current.stored  = (val == "true");
            else if (key == "fast")    current.fast    = (val == "true");
            else if (key == "multi")   current.multi   = (val == "true");
            else if (key == "sources") current.sources = parseSources(line);
        }
    }

    return schema;
}

// ── 加载（不存在则回退默认 Schema）──────────────────────────────────────────

Schema Schema::load(const std::string& index_dir) {
    std::string path = index_dir + "/schema.json";
    if (!std::filesystem::exists(path)) return defaultSchema();
    return fromJson(path);
}

// ── 默认 Schema（与现有硬编码行为等价）──────────────────────────────────────

Schema Schema::defaultSchema() {
    Schema s;
    s.fields = {
        {"title",     FieldType::Text,    IndexOption::FreqsPositions, true,  false},
        {"body",      FieldType::Text,    IndexOption::FreqsPositions, false, false},
        {"category",  FieldType::Keyword, IndexOption::None,           true,  false},
        {"source",    FieldType::Keyword, IndexOption::None,           true,  false},
        {"pubtime",   FieldType::Int64,   IndexOption::None,           false, true },
        {"uid",       FieldType::Int64,   IndexOption::None,           false, true },
        {"page_rank", FieldType::Float32, IndexOption::None,           false, true },
    };
    return s;
}

} // namespace ii
