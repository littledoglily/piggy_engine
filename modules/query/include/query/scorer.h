#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// query/scorer.h  —  Scorer 抽象基类
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"

namespace ii {

class Scorer {
public:
    virtual ~Scorer() = default;

    virtual DocId docId() const = 0;
    virtual bool  next()        = 0;
    virtual bool  advance(DocId target) = 0;
    virtual float score()       = 0;
    virtual float maxScore()    const = 0;

    virtual float blockMaxScore() const { return maxScore(); }
    virtual DocId blockMaxDocId() const { return INVALID_DOC; }

    virtual bool isEnd() const = 0;

    Scorer(const Scorer&)            = delete;
    Scorer& operator=(const Scorer&) = delete;
    Scorer()                         = default;
};

} // namespace ii
