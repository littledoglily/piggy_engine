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
// ─────────────────────────────────────────────────────────────────────────────

void MemorySegment::addDocument(const ii::Document& doc, ii::DocId doc_id) {
    // 1. 分词并建立倒排 posting
    uint32_t field_pos_base = 0;
    std::vector<ii::Token> token_buf;

    for (const auto& desc : descs_) {
        if (desc->indexOption() == ii::IndexOption::None) {
            // 无索引字段跳过分词
            token_buf.clear();  // 避免带入上一字段的 token
            field_pos_base = desc->buildTokensDoc(
                doc_id, doc.fields, field_pos_base, analyzer_, token_buf);
            continue;
        }

        token_buf.clear();
        field_pos_base = desc->buildTokensDoc(
            doc_id, doc.fields, field_pos_base, analyzer_, token_buf);

        if (!token_buf.empty())
            indexField(doc_id, desc->name(), token_buf);
    }

    // 2. 存储字段（stored=true 的字段写入 StoredPage）
    writeStoredFields(doc_id, doc);

    ++doc_count_;
}

// ─────────────────────────────────────────────────────────────────────────────
// indexField：为一个字段的 tokens 建立 posting entries
// ─────────────────────────────────────────────────────────────────────────────

void MemorySegment::indexField(ii::DocId doc_id,
                                const std::string& field_name,
                                const std::vector<ii::Token>& tokens)
{
    // 临时 per-term 聚合（栈上 map，doc 结束后随函数退出）
    // key = "field:term"，value = position 列表
    std::unordered_map<std::string, std::vector<uint32_t>> term_positions;

    for (const auto& tok : tokens) {
        std::string key = field_name + ":" + tok.term;
        term_positions[key].push_back(tok.position);
        ++total_tokens_;
    }

    // 分词完成后一次性提交所有 posting entry（避免半写状态进入 arena）
    for (auto& [key, positions] : term_positions) {
        uint32_t tf = static_cast<uint32_t>(positions.size());

        // 分配 TermPage
        uint32_t tp_off = allocTermPage(arena_);
        TermPage* tp    = arena_.at<TermPage>(tp_off);

        tp->doc_id = doc_id;
        tp->tf     = tf;

        // 获取当前 term 的链表头（新节点将成为新头）
        const Bucket* existing = hashtable_.lookup(key);
        tp->next = existing ? existing->term_page_head : INVALID_OFFSET;

        // 写入 position 信息
        if (tf <= static_cast<uint32_t>(INLINE_POS_LIMIT)) {
            // inline：直接写入 TermPage 内的 union
            for (uint32_t i = 0; i < tf; ++i)
                tp->inline_pos[i] = positions[i];
        } else {
            // 溢出：分配 PosPage 链
            tp->pos_head = writePosChain(positions);
        }

        // 更新 hashtable（upsert 会把 new_head 写入 bucket）
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
// 简单二进制格式（无压缩，Step 5 可替换为 LZ4）：
//   [n_fields : 4B]
//   For each stored field:
//     [name_len : 4B] [name : N bytes]
//     [val_len  : 4B] [val  : M bytes]
// ─────────────────────────────────────────────────────────────────────────────

void MemorySegment::writeStoredFields(ii::DocId doc_id, const ii::Document& doc) {
    // 1. 序列化到临时 buffer（栈上 string）
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

    // 2. 将 buf 写入 StoredPage 链
    const char* src      = buf.data();
    size_t      remaining = buf.size();
    uint32_t    head      = INVALID_OFFSET;
    uint32_t    prev_off  = INVALID_OFFSET;

    while (remaining > 0) {
        uint32_t sp_off          = allocStoredPage(arena_);
        StoredPageHeader* hdr    = arena_.at<StoredPageHeader>(sp_off);
        hdr->doc_id              = doc_id;
        hdr->next                = INVALID_OFFSET;

        size_t chunk = std::min(remaining, STORED_PAGE_DATA_SIZE);
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
    // head 暂不存储（Step 4 MemorySegmentReader 需要时再加 doc_id → stored_head 索引）
    (void)head;
}

// ─────────────────────────────────────────────────────────────────────────────
// reset：flush 后清空所有状态，arena 和 hashtable 复用
// ─────────────────────────────────────────────────────────────────────────────

void MemorySegment::reset() {
    arena_.reset();
    hashtable_.reset();
    doc_count_    = 0;
    total_tokens_ = 0;
}

} // namespace ii::memory
