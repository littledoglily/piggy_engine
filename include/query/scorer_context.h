#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// scorer_context.h  —  打分所需的全局上下文
//
// ScorerContext 由 IndexSearcher 在每个 Segment 的搜索入口处构造，
// 以 const 引用传入各 Scorer 节点，避免重复传参。
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include "segment/segment_reader.h"
#include <unordered_map>
#include <string>

namespace ii {

struct ScorerContext {
    const SegmentReader&                          seg;
    const std::unordered_map<std::string, float>& term_idfs;  // "field:term" → idf
    uint32_t                                      total_docs;  // 全局文档数（IDF 分母）
    const NumericFilter*                          filter;      // nullptr = 无过滤
    int                                           top_k = 10;  // WANDScorer TopK，BooleanQuery 路由时使用
};

} // namespace ii
