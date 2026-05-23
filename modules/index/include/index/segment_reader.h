#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index/segment_reader.h  —  只读方式打开并查询一个 Segment
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "index/i_segment_reader.h"
#include "codec/skiplist.h"
#include "store/i_posting_iterator.h"
#include "store/pos_iterator.h"
#include "field/fast_field_reader.h"
#include <string>
#include <map>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <bitset>
#include <memory>
#include <functional>

namespace ii {

class SegmentReader : public ISegmentReader {
public:
    static constexpr uint32_t MAX_DOCS = 65536;

    explicit SegmentReader(const std::string& dir, uint32_t segment_id);

    // ── ISegmentReader 接口（override）────────────────────────────────────────

    std::unique_ptr<IPostingIterator> postingIterator(const std::string& field,
                                                      const std::string& term) const override;

    const TermMeta* getTermMeta(const std::string& field,
                                const std::string& term) const override;

    uint32_t fieldDocLen   (const std::string& field, DocId doc_id) const override {
        auto it = field_doc_lens_.find(field);
        if (it == field_doc_lens_.end() || doc_id == 0 ||
            doc_id > static_cast<DocId>(it->second.size()))
            return 0;
        return it->second[doc_id - 1];
    }

    float fieldAvgDocLen(const std::string& field) const override {
        auto it = field_avg_doc_lens_.find(field);
        return it != field_avg_doc_lens_.end() ? it->second : 0.f;
    }

    uint32_t docCount()  const override { return doc_count_; }
    uint32_t segmentId() const override { return seg_id_; }

    const std::vector<std::string>& indexedFieldNames() const override {
        return indexed_field_names_;
    }

    bool isAlive(DocId doc_id) const override;

    // ── SegmentReader 专有接口（不在 ISegmentReader 中）──────────────────────

    void softDelete(DocId doc_id);

    // per-field .pos 读取（一次性加载全部，简单场景用）
    std::vector<PostingEntry> readPosEntries(const std::string& field,
                                             const std::string& term) const;

    // per-field .pos 流式迭代器（K-way merge / 大 df 词用，常数内存）
    PosIterator posIterator(const std::string& field,
                            const std::string& term) const;

    // per-field BM25（query 层用 TermScorer 计算，此处供调试）
    float bm25Score(DocId doc_id,
                    const std::string& field,
                    const std::vector<std::string>& query_terms,
                    const std::unordered_map<std::string, float>& term_idfs) const;

    // per-field 词典（merger 遍历字段全部 term）
    const std::map<std::string, TermMeta>* fieldTermDict(const std::string& field) const {
        auto it = field_term_dicts_.find(field);
        return it != field_term_dicts_.end() ? &it->second : nullptr;
    }

    StoredDocResult readStoredDoc(DocId doc_id) const override;

    uint32_t termCount() const { return static_cast<uint32_t>(term_dict_.size()); }

    // 暴露词典（工具 / 调试）
    const std::map<std::string, TermMeta>& termDict() const { return term_dict_; }

    // FastField 接口
    const FastFieldReader& ff() const { return *ff_; }
    bool hasFastField() const;
    int64_t ffPubtime  (uint32_t local_doc_idx) const override;
    int64_t ffUid      (uint32_t local_doc_idx) const override;
    float   ffPageRank (uint32_t local_doc_idx) const;
    std::vector<uint32_t> filterPubtime(int64_t lo, int64_t hi) const;
    std::vector<uint32_t> filterUid    (int64_t uid_val)        const;

    // ── 向后兼容 deprecated API ───────────────────────────────────────────────

    [[deprecated("use getTermMeta(field, term)")]]
    const TermMeta* getTermMeta(const std::string& term) const;

    std::vector<DocId>    readPostingList    (const std::string& term) const;
    std::vector<DocId>    readPostingListFrom(const std::string& term, DocId target) const;

    [[deprecated("use readPosEntries(field, term)")]]
    std::vector<PostingEntry> readPosEntries(const std::string& term) const;

    float bm25Score(DocId doc_id,
                    const std::vector<std::string>& query_terms,
                    const std::unordered_map<std::string, float>& term_idfs) const;

    SkipList readTermSkipList(const std::string& term) const {
        const TermMeta* m = getTermMeta(term);
        return m ? readSkipList(*m) : SkipList{};
    }

    [[deprecated("use postingIterator(field, term)")]]
    std::unique_ptr<IPostingIterator> postingIterator(const std::string& term) const;

private:
    void loadTim();
    void loadPerFieldTims(const std::string& indexed_fields_str);
    void loadFdx();
    void loadLiv();

    SkipList readSkipList     (const TermMeta& meta) const;
    SkipList readFieldSkipList(const std::string& field, const TermMeta& meta) const;

    std::ifstream& docFileForTerm(const std::string& term) const;

    std::string path      (const std::string& ext) const;
    std::string fieldPath (const std::string& field, const std::string& ext) const;

    std::string dir_;
    uint32_t    seg_id_;
    uint32_t    doc_count_    = 0;
    float       avg_doc_len_  = 0.0f;

    std::map<std::string, TermMeta>   term_dict_;
    std::vector<uint64_t>             fdx_offsets_;
    std::vector<bool>                 liv_bitmap_;

    std::map<std::string, std::map<std::string, TermMeta>> field_term_dicts_;
    std::map<std::string, float>                           field_avg_doc_lens_;
    std::map<std::string, std::vector<uint16_t>>           field_doc_lens_;
    std::vector<std::string>                               indexed_field_names_;
    mutable std::map<std::string, std::ifstream>           doc_files_;
    mutable std::map<std::string, std::ifstream>           per_field_pos_files_;

    mutable std::ifstream doc_file_;
    mutable std::ifstream pos_file_;
    mutable std::ifstream fdt_file_;

    std::unique_ptr<FastFieldReader> ff_;
};

} // namespace ii
