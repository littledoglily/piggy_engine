#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index/i_segment_reader.h  —  Segment 只读接口（query/ 依赖此接口，不依赖实现）
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "store/i_posting_iterator.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ii {

// 存储字段读取结果（disk: SegmentReader::readStoredDoc；memory: MemorySegmentReader::readStoredDoc）
struct StoredDocResult {
    DocId    doc_id = 0;
    uint64_t ext_id = 0;
    std::unordered_map<std::string, std::string> str_fields;

    const std::string& get(const std::string& k) const {
        static const std::string empty;
        auto it = str_fields.find(k);
        return it != str_fields.end() ? it->second : empty;
    }
    const std::string& source()   const { return get("source"); }
    const std::string& title()    const { return get("title"); }
    const std::string& body()     const { return get("body"); }
    const std::string& category() const { return get("category"); }
};

class ISegmentReader {
public:
    virtual ~ISegmentReader() = default;

    virtual std::unique_ptr<IPostingIterator> postingIterator(
        const std::string& field,
        const std::string& term) const = 0;

    virtual const TermMeta* getTermMeta(const std::string& field,
                                        const std::string& term) const = 0;

    virtual uint32_t fieldDocLen   (const std::string& field, DocId doc_id) const = 0;
    virtual float    fieldAvgDocLen(const std::string& field) const = 0;

    virtual uint32_t docCount()  const = 0;
    virtual uint32_t segmentId() const = 0;

    virtual const std::vector<std::string>& indexedFieldNames() const = 0;

    virtual bool isAlive(DocId doc_id) const = 0;

    // 存储字段读取（默认空，disk/memory 各自实现）
    virtual StoredDocResult readStoredDoc(DocId doc_id) const { return {}; }

    // FastField access for filter checks (default: no fast field)
    virtual int64_t ffPubtime(uint32_t doc_idx) const { return 0; }
    virtual int64_t ffUid    (uint32_t doc_idx) const { return 0; }
};

} // namespace ii
