#include "query/term_scorer.h"
#include "segment/segment_reader.h"

namespace ii {

TermScorer::TermScorer(const std::string& field,
                       const std::string& term,
                       const ScorerContext& ctx)
    : field_(field), term_(term), ctx_(&ctx)
{
    // legacy 分支使用 deprecated API，此处有意为之
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const TermMeta* meta = field_.empty()
        ? ctx.seg.getTermMeta(term_)
        : ctx.seg.getTermMeta(field_, term_);

    if (!meta) return;  // term 不存在，iter_ 保持默认（isEnd()==true）

    // IDF key："field:term" 或裸 term（legacy）
    const std::string idf_key = field_.empty() ? term_ : (field_ + ":" + term_);
    auto it = ctx.term_idfs.find(idf_key);
    idf_ = (it != ctx.term_idfs.end()) ? it->second : 0.0f;

    // 静态上界：max_tf_norm × IDF
    list_ub_ = meta->upper_bound * idf_;

    // 构造后迭代器已指向第一个 doc
    iter_ = field_.empty()
        ? ctx.seg.postingIterator(term_)
        : ctx.seg.postingIterator(field_, term_);
#pragma clang diagnostic pop
}

DocId TermScorer::docId()         const { return iter_.docId(); }
bool  TermScorer::next()                { return iter_.next(); }
bool  TermScorer::advance(DocId t)      { return iter_.advance(t); }
bool  TermScorer::isEnd()         const { return iter_.isEnd(); }
float TermScorer::maxScore()      const { return list_ub_; }
float TermScorer::blockMaxScore() const { return iter_.blockMaxScore() * idf_; }
DocId TermScorer::blockMaxDocId() const { return iter_.blockMaxDocId(); }

float TermScorer::score() {
    if (idf_ == 0.f) return 0.f;

    const float k1 = 1.2f, b = 0.75f;
    float tf_f  = static_cast<float>(iter_.tf());
    float avgdl = ctx_->seg.fieldAvgDocLen(field_);
    float dl    = static_cast<float>(ctx_->seg.fieldDocLen(field_, iter_.docId()));
    // avgdl=0 → 旧索引无 .len 文件，退化为 b=0
    float len_factor = (avgdl > 0.f) ? (1.0f - b + b * dl / avgdl) : 1.0f;
    return tf_f * (k1 + 1.0f) / (tf_f + k1 * len_factor) * idf_;
}

} // namespace ii
