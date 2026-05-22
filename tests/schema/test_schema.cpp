// tests/schema/test_schema.cpp
// Schema 单元测试：枚举转换、字段查询、持久化、defaultSchema
#include "../../tests/test_utils.h"
#include "field/schema.h"

#include <filesystem>
#include <cassert>
#include <string>

namespace fs = std::filesystem;
using namespace ii;

// ─────────────────────────────────────────────────────────────────────────────
// 辅助
// ─────────────────────────────────────────────────────────────────────────────
static std::string tmpDir(const std::string& suffix) {
    std::string dir = "/tmp/test_schema_" + suffix;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. 枚举 → 字符串
// ─────────────────────────────────────────────────────────────────────────────
void test_field_type_to_str() {
    TEST(field_type_to_str);
    assert(std::string(fieldTypeToStr(FieldType::Text))    == "text");
    assert(std::string(fieldTypeToStr(FieldType::Keyword)) == "keyword");
    assert(std::string(fieldTypeToStr(FieldType::Int64))   == "int64");
    assert(std::string(fieldTypeToStr(FieldType::Float32)) == "float32");
    PASS();
}

void test_index_option_to_str() {
    TEST(index_option_to_str);
    assert(std::string(indexOptionToStr(IndexOption::None))           == "none");
    assert(std::string(indexOptionToStr(IndexOption::DocsOnly))       == "docs_only");
    assert(std::string(indexOptionToStr(IndexOption::FreqsOnly))      == "freqs_only");
    assert(std::string(indexOptionToStr(IndexOption::FreqsPositions)) == "freqs_positions");
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. 字符串 → 枚举（含未知值回退）
// ─────────────────────────────────────────────────────────────────────────────
void test_field_type_from_str() {
    TEST(field_type_from_str);
    assert(fieldTypeFromStr("text")    == FieldType::Text);
    assert(fieldTypeFromStr("keyword") == FieldType::Keyword);
    assert(fieldTypeFromStr("int64")   == FieldType::Int64);
    assert(fieldTypeFromStr("float32") == FieldType::Float32);
    assert(fieldTypeFromStr("unknown") == FieldType::Text);  // 回退
    PASS();
}

void test_index_option_from_str() {
    TEST(index_option_from_str);
    assert(indexOptionFromStr("none")            == IndexOption::None);
    assert(indexOptionFromStr("docs_only")       == IndexOption::DocsOnly);
    assert(indexOptionFromStr("freqs_only")      == IndexOption::FreqsOnly);
    assert(indexOptionFromStr("freqs_positions") == IndexOption::FreqsPositions);
    assert(indexOptionFromStr("bad_value")       == IndexOption::None);  // 回退
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Schema::find()
// ─────────────────────────────────────────────────────────────────────────────
void test_schema_find() {
    TEST(schema_find);
    Schema s;
    s.fields = {
        {"title", FieldType::Text,    IndexOption::FreqsPositions, true,  false},
        {"uid",   FieldType::Int64,   IndexOption::None,           false, true },
    };

    const FieldSchema* f = s.find("title");
    assert(f != nullptr);
    assert(f->name   == "title");
    assert(f->type   == FieldType::Text);
    assert(f->stored == true);
    assert(f->fast   == false);

    const FieldSchema* g = s.find("uid");
    assert(g != nullptr);
    assert(g->fast == true);

    assert(s.find("nonexistent") == nullptr);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. indexedFields / storedFields / fastFields
// ─────────────────────────────────────────────────────────────────────────────
void test_schema_filter_methods() {
    TEST(schema_filter_methods);
    Schema s;
    s.fields = {
        // name         type              index                      stored  fast
        {"title",   FieldType::Text,    IndexOption::FreqsPositions, true,  false},
        {"body",    FieldType::Text,    IndexOption::FreqsOnly,      false, false},
        {"uid",     FieldType::Int64,   IndexOption::None,           false, true },
        {"source",  FieldType::Keyword, IndexOption::DocsOnly,       true,  false},
        {"rank",    FieldType::Float32, IndexOption::None,           false, true },
    };

    // indexedFields: title, body, source (index != None)
    auto idxd = s.indexedFields();
    assert(idxd.size() == 3);
    assert(idxd[0]->name == "title");
    assert(idxd[1]->name == "body");
    assert(idxd[2]->name == "source");

    // storedFields: title, source
    auto strd = s.storedFields();
    assert(strd.size() == 2);
    assert(strd[0]->name == "title");
    assert(strd[1]->name == "source");

    // fastFields: uid, rank
    auto fstd = s.fastFields();
    assert(fstd.size() == 2);
    assert(fstd[0]->name == "uid");
    assert(fstd[1]->name == "rank");

    PASS();
}

void test_schema_filter_empty() {
    TEST(schema_filter_empty);
    Schema s;  // 无字段
    assert(s.indexedFields().empty());
    assert(s.storedFields().empty());
    assert(s.fastFields().empty());
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. save() / fromJson() 往返
// ─────────────────────────────────────────────────────────────────────────────
void test_schema_save_load_roundtrip() {
    TEST(schema_save_load_roundtrip);
    auto dir = tmpDir("rw");

    Schema orig;
    orig.fields = {
        {"title",   FieldType::Text,    IndexOption::FreqsPositions, true,  false},
        {"body",    FieldType::Text,    IndexOption::FreqsOnly,      false, false},
        {"uid",     FieldType::Int64,   IndexOption::None,           false, true },
        {"rank",    FieldType::Float32, IndexOption::None,           false, true },
    };
    orig.save(dir);

    Schema loaded = Schema::fromJson(dir + "/schema.json");
    assert(loaded.fields.size() == orig.fields.size());

    for (size_t i = 0; i < orig.fields.size(); ++i) {
        const auto& o = orig.fields[i];
        const auto& l = loaded.fields[i];
        assert(l.name   == o.name);
        assert(l.type   == o.type);
        assert(l.index  == o.index);
        assert(l.stored == o.stored);
        assert(l.fast   == o.fast);
    }

    fs::remove_all(dir);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Schema::load() —— 文件存在时加载，不存在时回退 defaultSchema
// ─────────────────────────────────────────────────────────────────────────────
void test_schema_load_fallback() {
    TEST(schema_load_fallback);
    auto dir = tmpDir("fallback");

    // 目录下无 schema.json → 回退默认
    Schema s = Schema::load(dir);
    assert(!s.fields.empty());

    Schema def = Schema::defaultSchema();
    assert(s.fields.size() == def.fields.size());
    for (size_t i = 0; i < def.fields.size(); ++i) {
        assert(s.fields[i].name == def.fields[i].name);
    }

    fs::remove_all(dir);
    PASS();
}

void test_schema_load_existing() {
    TEST(schema_load_existing);
    auto dir = tmpDir("load");

    Schema orig;
    orig.fields = {{"score", FieldType::Float32, IndexOption::None, false, true}};
    orig.save(dir);

    Schema loaded = Schema::load(dir);
    assert(loaded.fields.size() == 1);
    assert(loaded.fields[0].name == "score");
    assert(loaded.fields[0].type == FieldType::Float32);
    assert(loaded.fields[0].fast == true);

    fs::remove_all(dir);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. defaultSchema() 内容验证
// ─────────────────────────────────────────────────────────────────────────────
void test_default_schema_content() {
    TEST(default_schema_content);
    Schema s = Schema::defaultSchema();

    // 必须包含这 7 个字段
    const char* expected[] = {"title","body","category","source","pubtime","uid","page_rank"};
    assert(s.fields.size() == 7);
    for (int i = 0; i < 7; ++i)
        assert(s.fields[i].name == expected[i]);

    // title / body 可建全文倒排
    assert(s.find("title")->index == IndexOption::FreqsPositions);
    assert(s.find("body")->index  == IndexOption::FreqsPositions);

    // title / category / source 存储原文
    assert(s.find("title")->stored    == true);
    assert(s.find("category")->stored == true);
    assert(s.find("source")->stored   == true);

    // pubtime / uid / page_rank 是 fast 字段
    assert(s.find("pubtime")->fast    == true);
    assert(s.find("uid")->fast        == true);
    assert(s.find("page_rank")->fast  == true);

    // body 不存储、不 fast
    assert(s.find("body")->stored == false);
    assert(s.find("body")->fast   == false);

    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. save() / load() 对所有枚举值的完整往返
// ─────────────────────────────────────────────────────────────────────────────
void test_schema_all_enum_values_roundtrip() {
    TEST(schema_all_enum_values_roundtrip);
    auto dir = tmpDir("enums");

    Schema orig;
    orig.fields = {
        {"f_text",    FieldType::Text,    IndexOption::FreqsPositions, true,  false},
        {"f_keyword", FieldType::Keyword, IndexOption::DocsOnly,       true,  false},
        {"f_int64",   FieldType::Int64,   IndexOption::FreqsOnly,      false, true },
        {"f_float32", FieldType::Float32, IndexOption::None,           false, true },
    };
    orig.save(dir);

    Schema s = Schema::fromJson(dir + "/schema.json");
    assert(s.fields[0].type  == FieldType::Text    && s.fields[0].index == IndexOption::FreqsPositions);
    assert(s.fields[1].type  == FieldType::Keyword && s.fields[1].index == IndexOption::DocsOnly);
    assert(s.fields[2].type  == FieldType::Int64   && s.fields[2].index == IndexOption::FreqsOnly);
    assert(s.fields[3].type  == FieldType::Float32 && s.fields[3].index == IndexOption::None);

    fs::remove_all(dir);
    PASS();
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    std::cout << "═══════════════════════════════════\n";
    std::cout << "  Schema Unit Tests\n";
    std::cout << "═══════════════════════════════════\n\n";

    // 枚举转换
    test_field_type_to_str();
    test_index_option_to_str();
    test_field_type_from_str();
    test_index_option_from_str();

    // 查询方法
    test_schema_find();
    test_schema_filter_methods();
    test_schema_filter_empty();

    // 持久化
    test_schema_save_load_roundtrip();
    test_schema_load_fallback();
    test_schema_load_existing();
    test_schema_all_enum_values_roundtrip();

    // 默认 Schema
    test_default_schema_content();

    std::cout << "\n═══════════════════════════════════\n";
    std::cout << "  All schema tests PASSED!\n";
    std::cout << "═══════════════════════════════════\n";
    return 0;
}
