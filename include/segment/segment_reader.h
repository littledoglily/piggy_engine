#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// segment_reader.h  —  只读方式打开并查询一个 Segment
//
// 模拟 Lucene 的 mmap 懒加载：
//   .tim  → 启动时全部读入 std::map（模拟 JVM 堆常驻）
//   .doc  → 按需 seek + read（模拟 mmap 缺页加载）
//   .fdt  → 命中后按偏移读取（模拟按需加载）
//   .liv  → 启动时读入 bitset（模拟常驻内存）
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include "postings/skiplist.h"
#include "postings/posting_iterator.h"
#include "fastfield/fast_field_reader.h"
#include <string>
#include <map>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <bitset>
#include <memory>
#include <functional>

namespace ii {

class SegmentReader {
public:
    static constexpr uint32_t MAX_DOCS = 65536;  // 示例上限

    explicit SegmentReader(const std::string& dir, uint32_t segment_id);

    // 检查文档是否存活（未被软删除）
    bool isAlive(DocId doc_id) const;

    // 软删除一个文档（修改 .liv 位图）
    void softDelete(DocId doc_id);

    // ── 字段级查询 API（per-field，Step 5+）────────────────────────────────────

    // 字段列表（构造时从 .si 解析，供查询层展开 bare term）
    const std::vector<std::string>& indexedFieldNames() const { return indexed_field_names_; }

    // 查找指定字段中 term 的元数据（未找到返回 nullptr）
    const TermMeta* getTermMeta(const std::string& field,
                                const std::string& term) const;

    // 惰性迭代器工厂（字段级）
    PostingIterator postingIterator(const std::string& field,
                                    const std::string& term) const;

    // 从 .pos_<field> 读取某字段某 term 的全部 <doc_id, tf, positions>
    std::vector<PostingEntry> readPosEntries(const std::string& field,
                                             const std::string& term) const;

    // 字段级 BM25（使用 per-field avg_doc_len）
    float bm25Score(DocId doc_id,
                    const std::string& field,
                    const std::vector<std::string>& query_terms,
                    const std::unordered_map<std::string, float>& term_idfs) const;

    // per-field avg_doc_len（由 TTF / doc_count 推算，构造时计算）
    float fieldAvgDocLen(const std::string& field) const {
        auto it = field_avg_doc_lens_.find(field);
        return it != field_avg_doc_lens_.end() ? it->second : 0.f;
    }

    // per-doc 字段长度（token 数）；未记录时返回 0
    uint32_t fieldDocLen(const std::string& field, DocId doc_id) const {
        auto it = field_doc_lens_.find(field);
        if (it == field_doc_lens_.end() || doc_id == 0 ||
            doc_id > static_cast<DocId>(it->second.size()))
            return 0;
        return it->second[doc_id - 1];
    }

    // ── 向后兼容 API（deprecated，不感知字段维度）──────────────────────────────

    // 查找 term 的元数据（合并跨字段，返回第一个命中字段的 meta）
    [[deprecated("use getTermMeta(field, term) for per-field lookup")]]
    const TermMeta* getTermMeta(const std::string& term) const;

    // 解压某 term 的完整 posting list（仅返回第一个命中字段的 docs）
    std::vector<DocId> readPostingList(const std::string& term) const;

    // 读取某 term 的 posting list 中满足 >= target_doc_id 的部分
    std::vector<DocId> readPostingListFrom(const std::string& term,
                                           DocId target_doc_id) const;

    // 从 .pos 读取某 term 所有文档（跨字段合并，legacy 兼容）
    [[deprecated("use readPosEntries(field, term) for per-field access")]]
    std::vector<PostingEntry> readPosEntries(const std::string& term) const;

    // 从 .fdt 按 doc_id 读取原文（返回 StoredDoc）
    struct StoredDocResult {
        DocId       doc_id = 0;
        uint64_t    ext_id = 0;
        std::unordered_map<std::string, std::string> str_fields;

        // 向后兼容访问器
        const std::string& source()   const { return get("source"); }
        const std::string& title()    const { return get("title"); }
        const std::string& body()     const { return get("body"); }
        const std::string& category() const { return get("category"); }

        const std::string& get(const std::string& k) const {
            static const std::string empty;
            auto it = str_fields.find(k);
            return it != str_fields.end() ? it->second : empty;
        }
    };
    StoredDocResult readStoredDoc(DocId doc_id) const;

    uint32_t docCount()  const { return doc_count_; }
    uint32_t termCount() const { return static_cast<uint32_t>(term_dict_.size()); }
    uint32_t segmentId() const { return seg_id_; }

    // 暴露词典（工具/调试用）
    const std::map<std::string, TermMeta>& termDict() const { return term_dict_; }

    // per-field 词典（merger 用：遍历某字段的全部 term）
    // legacy 模式下返回 nullptr
    const std::map<std::string, TermMeta>* fieldTermDict(const std::string& field) const {
        auto it = field_term_dicts_.find(field);
        return it != field_term_dicts_.end() ? &it->second : nullptr;
    }

    // 读取 term 的 SkipList（调试/统计用，legacy）
    SkipList readTermSkipList(const std::string& term) const {
        const TermMeta* m = getTermMeta(term);
        return m ? readSkipList(*m) : SkipList{};
    }

    // 惰性迭代器工厂（legacy，返回第一个命中字段的迭代器）
    [[deprecated("use postingIterator(field, term) for per-field iteration")]]
    PostingIterator postingIterator(const std::string& term) const;

    // 计算 BM25 score（单 doc，多 term）
    // term_idfs：由 IndexSearcher 基于全局统计预算的 {term -> global_idf}
    float bm25Score(DocId doc_id,
                    const std::vector<std::string>& query_terms,
                    const std::unordered_map<std::string, float>& term_idfs) const;

    // ── FastField 接口（数值列存）─────────────────────────────────────────────
    // 泛化访问：通过 FastFieldReader 泛化接口
    const FastFieldReader& ff() const { return *ff_; }
    bool hasFastField() const;

    // 向后兼容命名访问（local_doc_idx 为 0-indexed）
    int64_t ffPubtime(uint32_t local_doc_idx)  const;
    int64_t ffUid(uint32_t local_doc_idx)      const;
    float   ffPageRank(uint32_t local_doc_idx) const;

    // 范围过滤：返回满足条件的 local_doc_idx（0-indexed）
    std::vector<uint32_t> filterPubtime(int64_t lo, int64_t hi) const;
    std::vector<uint32_t> filterUid(int64_t uid_val)            const;

private:
    // 加载 .tim 词典到内存（自动检测 per-field 格式）
    void loadTim();
    // per-field 格式：从 indexed_fields_str 中的每个字段加载 _N.tim_<field>
    void loadPerFieldTims(const std::string& indexed_fields_str);

    // 加载 .fdx 偏移表
    void loadFdx();

    // 加载 .liv 位图
    void loadLiv();

    // 读取 term 对应的 SkipList（legacy，使用 doc_file_）
    SkipList readSkipList(const TermMeta& meta) const;
    // per-field SkipList 读取（使用 doc_files_[field]）
    SkipList readFieldSkipList(const std::string& field, const TermMeta& meta) const;

    // per-field 模式下找到包含 term 的字段文件句柄（legacy 接口路由用）
    // 若 legacy 模式或找不到，返回 doc_file_
    std::ifstream& docFileForTerm(const std::string& term) const;

    // 文件路径辅助
    std::string path(const std::string& ext) const;
    // _N.tim_title, _N.doc_body …
    std::string fieldPath(const std::string& field, const std::string& ext) const;

    std::string dir_;
    uint32_t    seg_id_;
    uint32_t    doc_count_ = 0;
    float       avg_doc_len_ = 0.0f;

    // 常驻内存（模拟 JVM 堆）
    std::map<std::string, TermMeta> term_dict_;   // 合并后的 .tim（legacy 或 per-field 合并）
    std::vector<uint64_t>           fdx_offsets_; // .fdx 全量加载
    std::vector<bool>               liv_bitmap_;  // .liv 全量加载

    // per-field 模式（Step 5+）
    std::map<std::string, std::map<std::string, TermMeta>> field_term_dicts_;
    std::map<std::string, float>                           field_avg_doc_lens_;
    std::map<std::string, std::vector<uint16_t>>           field_doc_lens_;   // field → [uint16_t × doc_count]
    std::vector<std::string>                               indexed_field_names_;
    mutable std::map<std::string, std::ifstream>           doc_files_;           // field → .doc_<field>
    mutable std::map<std::string, std::ifstream>           per_field_pos_files_; // field → .pos_<field>

    // 磁盘文件（按需 seek 读取）
    mutable std::ifstream doc_file_;   // legacy .doc  /  per-field 模式下指向第一个字段的 .doc
    mutable std::ifstream pos_file_;   // legacy .pos（per-field 模式下不使用）
    mutable std::ifstream fdt_file_;   // .fdt（共享）

    // 数值列存（构造时加载）
    std::unique_ptr<FastFieldReader> ff_;
};

} // namespace ii
