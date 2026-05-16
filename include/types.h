#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// types.h  —  基础类型定义
//
// 设计原则：
//   所有模块共用的 POD 结构体和别名集中在此，避免循环依赖。
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <string>
#include <vector>

namespace ii {

// ── 基础别名 ──────────────────────────────────────────────────────────────────
using DocId   = uint32_t;   // 文档 ID（1-indexed）
using TermFreq= uint32_t;   // term 在 doc 中出现次数
using Pos     = uint32_t;   // term 在 doc 中的词序位置

static constexpr DocId INVALID_DOC = 0xFFFFFFFFu;

// ── Token：分词器输出的最小单元 ───────────────────────────────────────────────
struct Token {
    std::string term;       // 词项字符串（已小写/词干化）
    DocId       doc_id;     // 所属文档
    Pos         position;   // 在文档中的第几个词（0-indexed）
    uint32_t    start_off;  // 原文字符起始偏移（高亮用）
    uint32_t    end_off;    // 原文字符结束偏移
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
};

// ── Document：写入索引的原始文档 ────────────────────────────────────────────
struct Document {
    // ── 标识符 ───────────────────────────────────────────────────────────────
    DocId       doc_id   = 0;     // 引擎内部顺序 ID（Segment 局部，1-indexed）
    uint64_t    ext_id   = 0;     // 外部数字 ID（wiki page id / ISBN number / URL hash 等）
    std::string source;           // 外部字符串标识（URL / DOI / ISBN / 文件路径等）

    // ── 正文字段 ─────────────────────────────────────────────────────────────
    std::string title;
    std::string body;
    std::string category;

    // ── 数值字段（FastField）─────────────────────────────────────────────────
    float       page_rank = 0.0f;
    int64_t     pubtime   = 0;    // Unix 时间戳，用于范围过滤 + 排序
    int64_t     uid       = 0;    // 用户 ID，用于等值过滤
};

// ── FastFieldDoc：数值列存缓冲（与 StoredDoc 并行，同 flush）────────────────
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
    uint64_t    ext_id  = 0;     // 外部数字 ID（原样从 .fdt 取回）
    std::string source;          // 外部字符串标识（URL 等，从 .fdt 取回）
    std::string title;
    int64_t     pubtime = 0;
    int64_t     uid     = 0;
};

} // namespace ii
