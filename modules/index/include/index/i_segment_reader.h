#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// index/i_segment_reader.h  —  Segment 只读接口（query/ 依赖此接口，不依赖实现）
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "store/posting_iterator.h"
#include <string>
#include <vector>

namespace ii {

class ISegmentReader {
public:
    virtual ~ISegmentReader() = default;

    virtual PostingIterator postingIterator(const std::string& field,
                                            const std::string& term) const = 0;

    virtual const TermMeta* getTermMeta(const std::string& field,
                                        const std::string& term) const = 0;

    virtual uint32_t fieldDocLen   (const std::string& field, DocId doc_id) const = 0;
    virtual float    fieldAvgDocLen(const std::string& field) const = 0;

    virtual uint32_t docCount()  const = 0;
    virtual uint32_t segmentId() const = 0;

    virtual const std::vector<std::string>& indexedFieldNames() const = 0;

    virtual bool isAlive(DocId doc_id) const = 0;
};

} // namespace ii
