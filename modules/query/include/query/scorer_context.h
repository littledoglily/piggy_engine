#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query/scorer_context.h  —  打分所需的全局上下文
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "index/i_segment_reader.h"
#include <unordered_map>
#include <string>

namespace ii {

struct ScorerContext {
    const ISegmentReader&                         seg;
    const std::unordered_map<std::string, float>& term_idfs;
    uint32_t                                      total_docs;
    const NumericFilter*                          filter;
    int                                           top_k = 10;
};

} // namespace ii
