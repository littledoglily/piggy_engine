#include "memory/memory_segment_reader.h"
#include "memory/memory_posting_iterator.h"
#include "memory/term_page.h"
#include "field/field_descriptor.h"

#include <algorithm>
#include <cmath>
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
//   - IDF = log(1 + (N - df + 0.5) / (df + 0.5))
//   - upper_bound: max BM25 tf_norm × IDF  (conservative: tf = ttf, dl = 1)
// ─────────────────────────────────────────────────────────────────────────────

const ii::TermMeta* MemorySegmentReader::getTermMeta(
    const std::string& field, const std::string& term) const
{
    std::string key = field + ":" + term;

    auto it = term_meta_cache_.find(key);
    if (it != term_meta_cache_.end()) return &it->second;

    const Bucket* b = seg_.hashtable().lookup(key);
    if (!b) return nullptr;

    uint32_t df    = b->doc_freq;
    uint32_t ttf   = b->total_tf;
    uint32_t N     = seg_.docCount();

    float idf = std::log(1.0f + ((float)N - (float)df + 0.5f) / ((float)df + 0.5f));

    // upper_bound: tf_norm at max tf, min dl=1
    float avg_dl = fieldAvgDocLen(field);
    if (avg_dl < 1.0f) avg_dl = 1.0f;
    float max_tf  = static_cast<float>(ttf);
    float tf_norm = max_tf * (BM25_K1 + 1.0f) /
                    (max_tf + BM25_K1 * (1.0f - BM25_B + BM25_B * 1.0f / avg_dl));

    ii::TermMeta meta{};
    meta.doc_freq        = df;
    meta.total_term_freq = ttf;
    meta.upper_bound     = tf_norm * idf;

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

} // namespace ii::memory
