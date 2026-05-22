#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// common/doc_ref.h  —  Document 的零拷贝传输包装
//
// DocRef = unique_ptr<Document>：
//   - 移动语义：Document 内的 string/vector 字段 move 为 O(1)，无深拷贝
//   - 调用方 std::move(doc) 传入，worker 接管所有权
//   - 线程安全：unique_ptr 保证同一时刻只有一个线程拥有 Document
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include <memory>

namespace ii {

using DocRef = std::unique_ptr<Document>;

inline DocRef makeDocRef(Document&& doc) {
    return std::make_unique<Document>(std::move(doc));
}

} // namespace ii
