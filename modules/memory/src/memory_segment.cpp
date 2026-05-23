#include "memory/memory_segment.h"
#include <cstring>
#include <stdexcept>

namespace ii::memory {

// ─────────────────────────────────────────────────────────────────────────────
// 构造
// ─────────────────────────────────────────────────────────────────────────────

MemorySegment::MemorySegment(const ii::Schema& schema,
                             const MemorySegmentConfig& cfg)
    : arena_(cfg.arena_bytes)
    , hashtable_(cfg.hashtable_cap, cfg.str_arena_bytes)
    , descs_(ii::buildDescriptors(schema))
{}

// ─────────────────────────────────────────────────────────────────────────────
// addDocument
// 顺序：先 writeStoredFields（预检 arena），再建倒排。
// 若 arena 不足，stored fields 写入前抛出，posting 尚未写入，状态干净。
// ─────────────────────────────────────────────────────────────────────────────

void MemorySegment::addDocument(const ii::Document& doc, ii::DocId doc_id) {
    // 1. 先写 stored fields
    writeStoredFields(doc_id, doc);

    // 2. 分词并建立倒排 posting（position offset 跨字段连续）
    uint32_t field_pos_base = 0;
    std::vector<ii::Token> token_buf;

    for (const auto& desc : descs_) {
        token_buf.clear();
        field_pos_base = desc->buildTokensDoc(
            doc_id, doc.fields, field_pos_base, analyzer_, token_buf);

        if (desc->indexOption() == ii::IndexOption::None) continue;

        if (!token_buf.empty())
            indexField(doc_id, desc->name(), token_buf);
    }

    ++doc_count_;
}

// ─────────────────────────────────────────────────────────────────────────────
// indexField：为一个字段的 tokens 建立 posting entries
// ─────────────────────────────────────────────────────────────────────────────

void MemorySegment::indexField(ii::DocId doc_id,
                                const std::string& field_name,
                                const std::vector<ii::Token>& tokens)
{
    // per-term 聚合：key = "field:term", value = position 列表
    std::unordered_map<std::string, std::vector<uint32_t>> term_positions;

    for (const auto& tok : tokens) {
        std::string key = field_name + ":" + tok.term;
        term_positions[key].push_back(tok.position);
        ++total_tokens_;
    }

    // 追踪 per-doc per-field token 数（BM25 fieldDocLen）
    doc_field_len_[doc_id][field_name] += static_cast<uint32_t>(tokens.size());
    field_total_tokens_[field_name]    += tokens.size();

    // 一次性提交所有 posting entry（避免半写状态进入 arena）
    for (auto& [key, positions] : term_positions) {
        uint32_t tf = static_cast<uint32_t>(positions.size());

        uint32_t tp_off = allocTermPage(arena_);
        TermPage* tp    = arena_.at<TermPage>(tp_off);

        tp->doc_id = doc_id;
        tp->tf     = tf;

        const Bucket* existing = hashtable_.lookup(key);
        tp->next = existing ? existing->term_page_head : INVALID_OFFSET;

        if (tf <= static_cast<uint32_t>(INLINE_POS_LIMIT)) {
            for (uint32_t i = 0; i < tf; ++i)
                tp->inline_pos[i] = positions[i];
        } else {
            tp->pos_head = writePosChain(positions);
        }

        hashtable_.upsert(key, tp_off, tf);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writePosChain：将 positions 写入一条 PosPage 链，返回链头 offset
// ─────────────────────────────────────────────────────────────────────────────

uint32_t MemorySegment::writePosChain(const std::vector<uint32_t>& positions) {
    uint32_t head     = INVALID_OFFSET;
    uint32_t prev_off = INVALID_OFFSET;

    for (size_t i = 0; i < positions.size(); ) {
        uint32_t pp_off = allocPosPage(arena_);
        PosPage* pp     = arena_.at<PosPage>(pp_off);
        pp->next        = INVALID_OFFSET;

        int count = 0;
        while (i < positions.size() && count < POS_PER_PAGE)
            pp->positions[count++] = positions[i++];

        if (head == INVALID_OFFSET) {
            head = pp_off;
        } else {
            arena_.at<PosPage>(prev_off)->next = pp_off;
        }
        prev_off = pp_off;
    }
    return head;
}

// ─────────────────────────────────────────────────────────────────────────────
// writeStoredFields：将存储字段序列化写入 StoredPage 链
//
// 格式（无压缩）：
//   [n_fields : 4B]
//   For each stored field:
//     [name_len : 4B] [name : N bytes] [val_len : 4B] [val : M bytes]
//
// 写入前预检 arena 空间，避免多页写到一半时 bad_alloc。
// ─────────────────────────────────────────────────────────────────────────────

void MemorySegment::writeStoredFields(ii::DocId doc_id, const ii::Document& doc) {
    // 1. 序列化到临时 buffer
    std::string buf;
    buf.reserve(256);

    uint32_t n_fields = 0;
    std::string fields_buf;
    for (const auto& desc : descs_) {
        if (!desc->isStored()) continue;
        auto it = doc.fields.find(desc->name());
        const ii::FieldVal* val = (it != doc.fields.end()) ? &it->second : nullptr;
        std::string s = desc->storeStr(val);
        if (s.empty()) continue;

        const std::string& name = desc->name();
        uint32_t name_len = static_cast<uint32_t>(name.size());
        uint32_t val_len  = static_cast<uint32_t>(s.size());

        fields_buf.append(reinterpret_cast<const char*>(&name_len), 4);
        fields_buf.append(name.data(), name_len);
        fields_buf.append(reinterpret_cast<const char*>(&val_len), 4);
        fields_buf.append(s.data(), val_len);
        ++n_fields;
    }

    if (n_fields == 0) return;

    buf.append(reinterpret_cast<const char*>(&n_fields), 4);
    buf.append(fields_buf);

    // 2. 预检 arena 空间（避免多页链写到一半 bad_alloc）
    size_t pages_needed = (buf.size() + STORED_PAGE_DATA_SIZE - 1) / STORED_PAGE_DATA_SIZE;
    if (arena_.full(pages_needed * STORED_PAGE_SIZE))
        throw std::bad_alloc();

    // 3. 将 buf 写入 StoredPage 链
    const char* src       = buf.data();
    size_t      remaining = buf.size();
    uint32_t    head      = INVALID_OFFSET;
    uint32_t    prev_off  = INVALID_OFFSET;

    while (remaining > 0) {
        uint32_t sp_off       = allocStoredPage(arena_);
        StoredPageHeader* hdr = arena_.at<StoredPageHeader>(sp_off);
        hdr->doc_id           = doc_id;
        hdr->next             = INVALID_OFFSET;

        size_t chunk  = std::min(remaining, STORED_PAGE_DATA_SIZE);
        hdr->data_len = static_cast<uint32_t>(chunk);

        uint8_t* data = reinterpret_cast<uint8_t*>(hdr) + sizeof(StoredPageHeader);
        std::memcpy(data, src, chunk);
        src       += chunk;
        remaining -= chunk;

        if (head == INVALID_OFFSET) {
            head = sp_off;
        } else {
            arena_.at<StoredPageHeader>(prev_off)->next = sp_off;
        }
        prev_off = sp_off;
    }

    // 记录 doc_id → stored head（MemorySegmentReader 读取 stored fields 需要）
    stored_heads_[doc_id] = head;
}

// ─────────────────────────────────────────────────────────────────────────────
// reset：flush 后清空所有状态，arena 和 hashtable 复用
// ─────────────────────────────────────────────────────────────────────────────

void MemorySegment::reset() {
    arena_.reset();
    hashtable_.reset();
    doc_count_         = 0;
    total_tokens_      = 0;
    doc_field_len_.clear();
    field_total_tokens_.clear();
    stored_heads_.clear();
}

} // namespace ii::memory
