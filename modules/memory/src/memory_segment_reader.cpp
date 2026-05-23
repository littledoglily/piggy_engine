#include "memory/memory_segment_reader.h"
#include "memory/memory_posting_iterator.h"
#include "memory/term_page.h"
#include "field/field_descriptor.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace ii::memory {

MemorySegmentReader::MemorySegmentReader(const MemorySegment& seg)
    : seg_(seg)
{
    for (const auto& desc : seg_.fieldDescs()) {
        if (desc->indexOption() != ii::IndexOption::None)
            field_names_.push_back(desc->name());
    }
}

MemorySegmentReader::MemorySegmentReader(std::shared_ptr<const MemorySegment> seg)
    : owned_seg_(std::move(seg)), seg_(*owned_seg_)
{
    for (const auto& desc : seg_.fieldDescs()) {
        if (desc->indexOption() != ii::IndexOption::None)
            field_names_.push_back(desc->name());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// postingIterator
//
// Walks TermPage chain (newest-first), collects (doc_id, tf), sorts ascending,
// and returns a MemoryPostingIterator (no disk I/O).
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<ii::IPostingIterator> MemorySegmentReader::postingIterator(
    const std::string& field, const std::string& term) const
{
    std::string key = field + ":" + term;
    const Bucket* b = seg_.hashtable().lookup(key);
    if (!b) return ii::IPostingIterator::makeEmpty();

    std::vector<ii::DocId>  docs;
    std::vector<uint32_t>   tfs;
    docs.reserve(b->doc_freq);
    tfs.reserve(b->doc_freq);

    uint32_t cur = b->term_page_head;
    while (cur != INVALID_OFFSET) {
        const TermPage* tp = seg_.arena().at<TermPage>(cur);
        docs.push_back(tp->doc_id);
        tfs.push_back(tp->tf);
        cur = tp->next;
    }

    // Sort (doc_id, tf) pairs by doc_id ascending
    std::vector<size_t> order(docs.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t bv){ return docs[a] < docs[bv]; });

    std::vector<ii::DocId>  sorted_docs(docs.size());
    std::vector<uint32_t>   sorted_tfs(tfs.size());
    for (size_t i = 0; i < order.size(); ++i) {
        sorted_docs[i] = docs[order[i]];
        sorted_tfs[i]  = tfs[order[i]];
    }

    const ii::TermMeta* meta = getTermMeta(field, term);
    float ub = meta ? meta->upper_bound : 100.0f;

    return std::make_unique<MemoryPostingIterator>(
        std::move(sorted_docs), std::move(sorted_tfs), ub);
}

// ─────────────────────────────────────────────────────────────────────────────
// getTermMeta
//
// Computes TermMeta from the hashtable Bucket:
//   - doc_freq / total_term_freq from bucket
//   - upper_bound: conservative max BM25 tf_norm (dl=1, tf=max per doc)
//     Does NOT include IDF — TermScorer multiplies IDF at query time,
//     matching the on-disk TermMeta.upper_bound convention.
// ─────────────────────────────────────────────────────────────────────────────

const ii::TermMeta* MemorySegmentReader::getTermMeta(
    const std::string& field, const std::string& term) const
{
    std::string key = field + ":" + term;

    std::lock_guard lock(term_meta_mutex_);

    auto it = term_meta_cache_.find(key);
    if (it != term_meta_cache_.end()) return &it->second;

    const Bucket* b = seg_.hashtable().lookup(key);
    if (!b) return nullptr;

    uint32_t df    = b->doc_freq;
    uint32_t ttf   = b->total_tf;

    // upper_bound = max tf_norm across all docs (conservative: dl=1, which maximises tf_norm).
    // IDF is NOT included here — callers (TermScorer) multiply it in at query time.
    float avg_dl = fieldAvgDocLen(field);
    if (avg_dl < 1.0f) avg_dl = 1.0f;
    float max_tf  = static_cast<float>(ttf);
    float tf_norm = max_tf * (BM25_K1 + 1.0f) /
                    (max_tf + BM25_K1 * (1.0f - BM25_B + BM25_B * 1.0f / avg_dl));

    ii::TermMeta meta{};
    meta.doc_freq        = df;
    meta.total_term_freq = ttf;
    meta.upper_bound     = tf_norm;

    auto [ins, _] = term_meta_cache_.emplace(key, meta);
    return &ins->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// fieldDocLen / fieldAvgDocLen
// ─────────────────────────────────────────────────────────────────────────────

uint32_t MemorySegmentReader::fieldDocLen(
    const std::string& field, ii::DocId doc_id) const
{
    return seg_.docFieldLen(field, doc_id);
}

float MemorySegmentReader::fieldAvgDocLen(const std::string& field) const
{
    uint32_t n = seg_.docCount();
    if (n == 0) return 1.0f;
    return static_cast<float>(seg_.fieldTotalTokens(field)) / static_cast<float>(n);
}

// ─────────────────────────────────────────────────────────────────────────────
// readStoredDoc：从 StoredPage 链还原存储字段
//
// 内存格式（writeStoredFields 写入）：
//   [n_fields:4B] [name_len:4B name:N val_len:4B val:M] ...
// ─────────────────────────────────────────────────────────────────────────────

ii::StoredDocResult MemorySegmentReader::readStoredDoc(ii::DocId doc_id) const
{
    ii::StoredDocResult result;
    result.doc_id = doc_id;
    result.ext_id = seg_.docExtId(doc_id);

    uint32_t head = seg_.storedHead(doc_id);
    if (head == INVALID_OFFSET) return result;

    // 拼接 StoredPage 链
    std::string payload;
    uint32_t cur = head;
    while (cur != INVALID_OFFSET) {
        const StoredPageHeader* hdr = seg_.arena().at<StoredPageHeader>(cur);
        const char* data = reinterpret_cast<const char*>(hdr) + sizeof(StoredPageHeader);
        payload.append(data, hdr->data_len);
        cur = hdr->next;
    }
    if (payload.size() < 4) return result;

    const char* p   = payload.data();
    const char* end = p + payload.size();

    uint32_t n_fields = 0;
    std::memcpy(&n_fields, p, 4); p += 4;

    for (uint32_t i = 0; i < n_fields && p + 8 <= end; ++i) {
        uint32_t name_len = 0;
        std::memcpy(&name_len, p, 4); p += 4;
        if (p + name_len > end) break;
        std::string name(p, name_len); p += name_len;
        if (p + 4 > end) break;
        uint32_t val_len = 0;
        std::memcpy(&val_len, p, 4); p += 4;
        if (p + val_len > end) break;
        std::string val(p, val_len); p += val_len;
        result.str_fields[std::move(name)] = std::move(val);
    }

    return result;
}

} // namespace ii::memory
