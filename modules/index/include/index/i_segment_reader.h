#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index/i_segment_reader.h  —  Segment 只读接口（query/ 依赖此接口，不依赖实现）
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "store/i_posting_iterator.h"
#include <memory>
#include <string>
#include <vector>

namespace ii {

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

    // FastField access for filter checks in WANDScorer (default: no fast field)
    virtual int64_t ffPubtime(uint32_t doc_idx) const { return 0; }
    virtual int64_t ffUid    (uint32_t doc_idx) const { return 0; }
};

} // namespace ii
