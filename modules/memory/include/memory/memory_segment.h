#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// memory/memory_segment.h  —  实时写入核心（Step 3）
//
// 职责：
//   - 接收文档，分词后将 posting entry 写入 arena（TermPage / PosPage）
//   - 存储字段序列化到 StoredPage
//   - 维护 TermHashTable（term → posting list 头）
//   - 提供 shouldFlush() 供调用方判断是否需要 flush 到磁盘
//
// 线程安全：
//   NOT thread-safe。设计上由单一 IndexWorker 独占写入（per-segment-per-thread）。
// ─────────────────────────────────────────────────────────────────────────────
#include "memory/segment_arena.h"
#include "memory/term_hash_table.h"
#include "memory/term_page.h"
#include "field/field_descriptor.h"
#include "field/schema.h"
#include "core/types.h"
#include "analysis/analyzer.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ii::memory {

// ─── MemorySegment::Config ────────────────────────────────────────────────────

struct MemorySegmentConfig {
    size_t arena_bytes     = 64ULL * 1024 * 1024;  // TermPage / PosPage / StoredPage 池
    size_t str_arena_bytes = 8ULL  * 1024 * 1024;  // term string 存储
    size_t hashtable_cap   = 131072;                // 2^17，需为 2 的幂
};

// ─── MemorySegment ────────────────────────────────────────────────────────────

class MemorySegment {
public:
    explicit MemorySegment(const ii::Schema& schema,
                           const MemorySegmentConfig& cfg = {});

    // 禁止拷贝/移动（持有 arena 大块内存）
    MemorySegment(const MemorySegment&)            = delete;
    MemorySegment& operator=(const MemorySegment&) = delete;

    // 将一篇文档写入内存索引。
    // doc_id 由调用方（IndexWorker / 全局原子计数器）提供，不在此处分配。
    void addDocument(const ii::Document& doc, ii::DocId doc_id);

    // ── 状态查询 ─────────────────────────────────────────────────────────────
    uint32_t docCount()  const { return doc_count_; }
    size_t   ramUsed()   const { return arena_.used(); }
    size_t   ramCap()    const { return arena_.capacity(); }

    // flush 触发条件：arena 空间不足 或 hashtable 负载过高
    bool shouldFlush() const {
        return arena_.full(MIN_RESERVE_BYTES) || hashtable_.needsFlush();
    }

    // ── 供 MemorySegmentReader 和 flush 路径只读访问 ──────────────────────────
    const TermHashTable& hashtable() const { return hashtable_; }
    const SegmentArena&  arena()     const { return arena_; }

    // reset 供 flush 后复用（清空所有状态）
    void reset();

private:
    // 为 doc 中 field 的 token list 建立 posting entries
    void indexField(ii::DocId doc_id,
                    const std::string& field_name,
                    const std::vector<ii::Token>& tokens);

    // 将 positions 写入 PosPage 链，返回链头 offset
    uint32_t writePosChain(const std::vector<uint32_t>& positions);

    // 将存储字段内容序列化写入 StoredPage 链
    // 格式：[n_fields:4B] [name_len:4B name:N val_len:4B val:M] ...
    void writeStoredFields(ii::DocId doc_id, const ii::Document& doc);

    // ── 组件 ─────────────────────────────────────────────────────────────────
    SegmentArena  arena_;
    TermHashTable hashtable_;
    ii::Analyzer  analyzer_;

    std::vector<std::unique_ptr<ii::FieldDescriptor>> descs_;

    // ── 统计 ─────────────────────────────────────────────────────────────────
    uint32_t doc_count_       = 0;
    uint64_t total_tokens_    = 0;   // 所有字段所有文档的 token 总数（用于 avgdl）

    // arena 中至少要为一篇文档预留的空间（估算：最大文档 ~100 term，每 term 2 pages）
    static constexpr size_t MIN_RESERVE_BYTES = 64 * 1024;
};

} // namespace ii::memory
