#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// core/types.h  —  基础类型定义
//
// 设计原则：
//   所有模块共用的 POD 结构体和别名集中在此，避免循环依赖。
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>

namespace ii {

// ── 基础别名 ──────────────────────────────────────────────────────────────────
using DocId   = uint32_t;   // 文档 ID（1-indexed）
using TermFreq= uint32_t;   // term 在 doc 中出现次数
using Pos     = uint32_t;   // term 在 doc 中的词序位置

static constexpr DocId INVALID_DOC = 0xFFFFFFFFu;

// ── FieldVal：字段值的通用变体类型 ───────────────────────────────────────────
// 定长单值：int64_t（Int64 字段）、float（Float32 字段）
// 变长单值：std::string（Text / Keyword / Stored 字段）
// 变长多值：std::vector<std::string>（多值 Keyword / Tag 字段）
using FieldVal = std::variant<std::string, int64_t, float, std::vector<std::string>>;

// ── Token：分词器输出的最小单元 ───────────────────────────────────────────────
struct Token {
    std::string term;       // 词项字符串（已小写/词干化）
    DocId       doc_id;     // 所属文档
    Pos         position;   // 在文档中的第几个词（0-indexed）
    uint32_t    start_off;  // 原文字符起始偏移（高亮用）
    uint32_t    end_off;    // 原文字符结束偏移
    std::string field;      // 产生该 token 的字段名（与 FieldSchema.name 对应）
};

// ── PostingEntry：倒排链中一个 <doc, tf, positions> 节点 ─────────────────────
struct PostingEntry {
    DocId              doc_id;
    TermFreq           tf;
    std::vector<Pos>   positions;  // 该 term 在 doc 中所有出现位置
};

// ── SkipNode：跳表节点，对应一个 128-doc Block ───────────────────────────────
struct SkipNode {
    DocId    max_doc_id;    // 本 Block 最大 doc_id
    uint64_t byte_offset;   // 本 Block 在 .doc 文件 posting 区的字节偏移
    float    max_score;     // 本 Block 内最高 BM25 贡献分（BMW/WAND 用）
    uint32_t doc_count;     // 本 Block 实际 doc 数（最后一块可能 < 128）
};

// ── TermMeta：词典中每个 term 的元数据 ──────────────────────────────────────
struct TermMeta {
    uint32_t  doc_freq;         // df：出现该 term 的文档数
    uint32_t  total_term_freq;  // 所有文档中该 term 出现次数之和
    uint64_t  posting_offset;   // .doc 文件中该 term posting 区起始偏移
    uint64_t  skip_offset;      // .doc 文件中 SkipList 起始偏移
    uint64_t  pos_offset;       // .pos 文件中位置信息起始偏移
    float     upper_bound;      // UB：该 term 最大 BM25 贡献分（WAND 剪枝）
    uint64_t  tf_data_offset = 0; // .doc 文件中 tf 字节数组起始偏移（0 = 未存储）
};

// ── Document：写入索引的原始文档 ─────────────────────────────────────────────
// doc_id / ext_id 是引擎保留的身份字段；业务字段通过 fields map 统一管理，
// 使用 set() 赋值，getString() / getInt64() / getFloat() 读取。
struct Document {
    DocId    doc_id = 0;   // 引擎内部顺序 ID（Segment 局部，1-indexed）
    uint64_t ext_id = 0;   // 外部数字 ID（wiki page id / ISBN / URL hash 等）

    std::unordered_map<std::string, FieldVal> fields;

    // ── 赋值接口（支持链式调用）──────────────────────────────────────────────
    Document& set(const std::string& name, std::string v) {
        fields[name] = std::move(v); return *this;
    }
    Document& set(const std::string& name, int64_t v) {
        fields[name] = v; return *this;
    }
    Document& set(const std::string& name, float v) {
        fields[name] = v; return *this;
    }
    Document& set(const std::string& name, std::vector<std::string> v) {
        fields[name] = std::move(v); return *this;
    }

    // ── 读取接口 ─────────────────────────────────────────────────────────────
    const std::string& getString(const std::string& name) const {
        static const std::string kEmpty;
        auto it = fields.find(name);
        if (it == fields.end()) return kEmpty;
        const std::string* p = std::get_if<std::string>(&it->second);
        return p ? *p : kEmpty;
    }
    int64_t getInt64(const std::string& name, int64_t def = 0) const {
        auto it = fields.find(name);
        if (it == fields.end()) return def;
        const int64_t* p = std::get_if<int64_t>(&it->second);
        return p ? *p : def;
    }
    float getFloat(const std::string& name, float def = 0.0f) const {
        auto it = fields.find(name);
        if (it == fields.end()) return def;
        const float* p = std::get_if<float>(&it->second);
        return p ? *p : def;
    }
    const std::vector<std::string>* getStrList(const std::string& name) const {
        auto it = fields.find(name);
        if (it == fields.end()) return nullptr;
        return std::get_if<std::vector<std::string>>(&it->second);
    }
};

// ── FastFieldDoc：向后兼容，供 FastFieldWriter::add() 直接调用 ───────────────
struct FastFieldDoc {
    int64_t pubtime   = 0;
    int64_t uid       = 0;
    float   page_rank = 0.0f;
};

// ── NumericFilter：数值字段过滤条件 ─────────────────────────────────────────
struct NumericFilter {
    int64_t pubtime_lo = INT64_MIN;   // pubtime >= lo
    int64_t pubtime_hi = INT64_MAX;   // pubtime <= hi
    int64_t uid        = -1;          // -1 表示不过滤 uid
    bool    sort_by_pubtime = false;  // true 则结果按 pubtime 降序排列

    bool hasPubtimeRange() const {
        return pubtime_lo != INT64_MIN || pubtime_hi != INT64_MAX;
    }
    bool hasUidFilter() const { return uid >= 0; }
    bool hasAnyFilter() const { return hasPubtimeRange() || hasUidFilter(); }
};

// ── SearchResult：单条搜索结果 ───────────────────────────────────────────────
struct SearchResult {
    DocId       doc_id  = 0;
    float       score   = 0.0f;
    uint64_t    ext_id  = 0;
    int64_t     pubtime = 0;
    int64_t     uid     = 0;

    // 向后兼容的命名字段（同时也写入 stored_fields）
    std::string source;
    std::string title;

    // 所有 stored:true 字段的通用 map（含 source / title）
    std::unordered_map<std::string, std::string> stored_fields;
};

} // namespace ii
