#include "memory/memory_segment.h"
#include "codec/pfor_delta.h"
#include "codec/skiplist.h"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
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
    doc_ext_ids_[doc_id] = doc.ext_id;

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
    doc_ext_ids_.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// flushToDisk：将内存 segment 序列化为磁盘格式
//
// 写出格式与 SegmentWriter::flush() 完全兼容（SegmentReader 可直接打开）。
// 调用方应在此方法返回后调用 reset() 复用 MemorySegment。
// ─────────────────────────────────────────────────────────────────────────────

void MemorySegment::flushToDisk(const std::string& dir, uint32_t seg_id) const {
    namespace fs = std::filesystem;

    const std::string seg_dir = dir + "/segment_" + std::to_string(seg_id);
    fs::create_directories(seg_dir);
    { std::ofstream ing(seg_dir + "/.ing"); }

    // ── 本地写辅助 ───────────────────────────────────────────────────────────
    auto segPath   = [&](const std::string& ext) { return seg_dir + "/" + ext; };
    auto fieldPath = [&](const std::string& f, const std::string& ext) {
        return seg_dir + "/" + ext + "_" + f;
    };
    auto wU32 = [](std::ofstream& f, uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto wU64 = [](std::ofstream& f, uint64_t v) { f.write(reinterpret_cast<const char*>(&v), 8); };
    auto wF32 = [](std::ofstream& f, float    v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto wStr = [&](std::ofstream& f, const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        wU32(f, len);
        f.write(s.data(), len);
    };

    // ── 结构：每个 term 的排序后 entries ────────────────────────────────────
    struct Entry {
        ii::DocId             doc_id;
        uint32_t              tf;
        std::vector<uint32_t> positions;
    };
    struct TermData {
        uint32_t         df;
        uint32_t         ttf;
        std::vector<Entry> entries;  // sorted by doc_id ascending
    };

    // ── 1. 遍历 hashtable，按 field 分组 ─────────────────────────────────────
    std::map<std::string, std::map<std::string, TermData>> field_terms;

    hashtable_.forEach([&](const Bucket& b) {
        std::string_view key = hashtable_.termString(b);
        auto colon = key.find(':');
        if (colon == std::string_view::npos) return;
        std::string field(key.substr(0, colon));
        std::string term (key.substr(colon + 1));

        TermData td;
        td.df  = b.doc_freq;
        td.ttf = b.total_tf;

        // Traverse TermPage chain (newest-first), collect entries
        uint32_t cur = b.term_page_head;
        while (cur != INVALID_OFFSET) {
            const TermPage* tp = arena_.at<TermPage>(cur);
            Entry e;
            e.doc_id = tp->doc_id;
            e.tf     = tp->tf;

            if (tp->tf <= static_cast<uint32_t>(INLINE_POS_LIMIT)) {
                for (uint32_t i = 0; i < tp->tf; ++i)
                    e.positions.push_back(tp->inline_pos[i]);
            } else {
                uint32_t pp = tp->pos_head;
                while (pp != INVALID_OFFSET) {
                    const PosPage* pg = arena_.at<PosPage>(pp);
                    size_t want = e.tf - static_cast<uint32_t>(e.positions.size());
                    size_t n    = std::min(want, static_cast<size_t>(POS_PER_PAGE));
                    for (size_t i = 0; i < n; ++i)
                        e.positions.push_back(pg->positions[i]);
                    pp = pg->next;
                }
            }

            td.entries.push_back(std::move(e));
            cur = tp->next;
        }

        // Sort by doc_id ascending
        std::sort(td.entries.begin(), td.entries.end(),
                  [](const Entry& a, const Entry& b) { return a.doc_id < b.doc_id; });

        field_terms[field][term] = std::move(td);
    });

    // ── 2. 按字段写磁盘文件 ─────────────────────────────────────────────────
    const float k1 = 1.2f, b = 0.75f;
    constexpr size_t BLOCK_SIZE = 128;

    std::vector<std::string> written_fields;
    uint32_t total_term_count = 0;

    for (auto& [field, terms] : field_terms) {
        if (terms.empty()) continue;
        written_fields.push_back(field);

        // avgdl for this field
        uint64_t total_tok = 0;
        {
            auto it = field_total_tokens_.find(field);
            if (it != field_total_tokens_.end()) total_tok = it->second;
        }
        float avgdl = (doc_count_ > 0) ? static_cast<float>(total_tok) / doc_count_ : 0.f;

        // ── BM25 tf_norm per term ─────────────────────────────────────────────
        struct NormData { float global_max; std::vector<float> block_maxes; };
        std::map<std::string, NormData> tf_norms;

        for (auto& [term, td] : terms) {
            NormData nd;
            nd.global_max = 0.f;
            for (size_t i = 0; i < td.entries.size(); i += BLOCK_SIZE) {
                size_t end = std::min(i + BLOCK_SIZE, td.entries.size());
                float blk_max = 0.f;
                for (size_t j = i; j < end; ++j) {
                    const auto& e = td.entries[j];
                    float dl = 0.f;
                    {
                        auto it = doc_field_len_.find(e.doc_id);
                        if (it != doc_field_len_.end()) {
                            auto jt = it->second.find(field);
                            if (jt != it->second.end()) dl = static_cast<float>(jt->second);
                        }
                    }
                    float tf_f = static_cast<float>(e.tf);
                    float norm = tf_f * (k1 + 1.f) /
                                 (tf_f + k1 * (1.f - b + b * (avgdl > 0.f ? dl / avgdl : 1.f)));
                    blk_max = std::max(blk_max, norm);
                }
                nd.block_maxes.push_back(blk_max);
                nd.global_max = std::max(nd.global_max, blk_max);
            }
            tf_norms[term] = std::move(nd);
        }

        // ── Write .tim_<field> ────────────────────────────────────────────────
        std::map<std::string, uint64_t> tim_patch;
        std::map<std::string, ii::TermMeta> term_meta;

        {
            std::ofstream ftim(fieldPath(field, "tim"), std::ios::binary | std::ios::trunc);
            wU32(ftim, static_cast<uint32_t>(terms.size()));
            for (auto& [term, td] : terms) {
                ii::TermMeta meta{};
                meta.doc_freq        = td.df;
                meta.total_term_freq = td.ttf;
                meta.upper_bound     = tf_norms[term].global_max;
                term_meta[term]      = meta;

                wStr(ftim, term);
                wU32(ftim, meta.doc_freq);
                wU32(ftim, meta.total_term_freq);
                tim_patch[term] = static_cast<uint64_t>(ftim.tellp());
                wU64(ftim, 0);   // posting_offset (+0)
                wU64(ftim, 0);   // skip_offset    (+8)
                wU64(ftim, 0);   // pos_offset     (+16)
                wF32(ftim, meta.upper_bound);
                wU64(ftim, 0);   // tf_data_offset (+28)
            }
        }

        // ── Write .doc_<field> ────────────────────────────────────────────────
        {
            std::ofstream fdoc(fieldPath(field, "doc"), std::ios::binary | std::ios::trunc);
            wU32(fdoc, static_cast<uint32_t>(terms.size()));

            for (auto& [term, td] : terms) {
                auto& meta = term_meta[term];

                std::vector<ii::DocId> doc_ids;
                doc_ids.reserve(td.entries.size());
                for (const auto& e : td.entries) doc_ids.push_back(e.doc_id);

                std::vector<ii::SkipNode> skip_nodes;
                auto compressed = ii::PForDelta::compress(doc_ids, skip_nodes,
                                                           tf_norms[term].block_maxes);

                ii::SkipList sl(skip_nodes);
                auto sl_bytes = sl.serialize();

                uint64_t cur_pos     = static_cast<uint64_t>(fdoc.tellp());
                meta.skip_offset     = cur_pos;
                meta.posting_offset  = cur_pos + static_cast<uint64_t>(sl_bytes.size());

                fdoc.write(reinterpret_cast<const char*>(sl_bytes.data()), sl_bytes.size());
                fdoc.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());

                meta.tf_data_offset  = static_cast<uint64_t>(fdoc.tellp());
                for (const auto& e : td.entries) {
                    uint8_t tf_byte = static_cast<uint8_t>(std::min(e.tf, 255u));
                    fdoc.write(reinterpret_cast<const char*>(&tf_byte), 1);
                }
            }

            // Patch .tim: posting_offset (+0) and skip_offset (+8) and tf_data_offset (+28)
            std::fstream ftim(fieldPath(field, "tim"),
                              std::ios::binary | std::ios::in | std::ios::out);
            for (const auto& [term, meta] : term_meta) {
                uint64_t base = tim_patch.at(term);
                ftim.seekp(static_cast<std::streamoff>(base));
                ftim.write(reinterpret_cast<const char*>(&meta.posting_offset), 8);
                ftim.write(reinterpret_cast<const char*>(&meta.skip_offset),    8);
                ftim.seekp(static_cast<std::streamoff>(base + 28));
                ftim.write(reinterpret_cast<const char*>(&meta.tf_data_offset), 8);
            }
        }

        // ── Write .pos_<field> ────────────────────────────────────────────────
        {
            std::ofstream fpos(fieldPath(field, "pos"), std::ios::binary | std::ios::trunc);
            for (auto& [term, td] : terms) {
                term_meta[term].pos_offset = static_cast<uint64_t>(fpos.tellp());
                for (const auto& e : td.entries) {
                    wU32(fpos, e.doc_id);
                    wU32(fpos, e.tf);
                    for (uint32_t p : e.positions) wU32(fpos, p);
                }
            }

            // Patch .tim: pos_offset at +16
            std::fstream ftim(fieldPath(field, "tim"),
                              std::ios::binary | std::ios::in | std::ios::out);
            for (const auto& [term, meta] : term_meta) {
                uint64_t pos_off = meta.pos_offset;
                ftim.seekp(static_cast<std::streamoff>(tim_patch.at(term) + 16));
                ftim.write(reinterpret_cast<const char*>(&pos_off), 8);
            }
        }

        // ── Write .len_<field> ────────────────────────────────────────────────
        {
            std::ofstream flen(fieldPath(field, "len"), std::ios::binary | std::ios::trunc);
            wU32(flen, doc_count_);
            for (uint32_t i = 0; i < doc_count_; ++i) {
                ii::DocId did = i + 1;
                uint32_t  len = 0;
                auto it = doc_field_len_.find(did);
                if (it != doc_field_len_.end()) {
                    auto jt = it->second.find(field);
                    if (jt != it->second.end()) len = jt->second;
                }
                uint16_t len16 = static_cast<uint16_t>(std::min(len, 65535u));
                flen.write(reinterpret_cast<const char*>(&len16), 2);
            }
        }

        total_term_count += static_cast<uint32_t>(terms.size());
    }

    // ── 3. Write .fdt / .fdx ─────────────────────────────────────────────────
    {
        std::ofstream ffdt(segPath("fdt"), std::ios::binary | std::ios::trunc);
        std::vector<std::pair<ii::DocId, uint64_t>> fdx_entries;

        for (uint32_t i = 0; i < doc_count_; ++i) {
            ii::DocId did = i + 1;

            // Always write fdt entry: [doc_id:4B] [ext_id:8B] [n_fields:4B] [fields...]
            // This ensures ext_id is readable via readStoredDoc even when no fields are stored.
            fdx_entries.push_back({did, static_cast<uint64_t>(ffdt.tellp())});
            wU32(ffdt, did);
            uint64_t ext_id = 0;
            {
                auto eit = doc_ext_ids_.find(did);
                if (eit != doc_ext_ids_.end()) ext_id = eit->second;
            }
            wU64(ffdt, ext_id);

            auto it = stored_heads_.find(did);
            if (it == stored_heads_.end()) {
                wU32(ffdt, 0);  // n_fields = 0
                continue;
            }

            // Reconstruct stored buffer from StoredPage chain
            std::string payload;
            uint32_t cur = it->second;
            while (cur != INVALID_OFFSET) {
                const StoredPageHeader* hdr = arena_.at<StoredPageHeader>(cur);
                const char* data = reinterpret_cast<const char*>(hdr) + sizeof(StoredPageHeader);
                payload.append(data, hdr->data_len);
                cur = hdr->next;
            }
            if (payload.size() < 4) {
                wU32(ffdt, 0);  // n_fields = 0
                continue;
            }

            // In-memory format: [n_fields:4B] [name_len:4B name val_len:4B val] ...
            uint32_t n_fields = 0;
            std::memcpy(&n_fields, payload.data(), 4);
            wU32(ffdt, n_fields);
            ffdt.write(payload.data() + 4, payload.size() - 4);
        }

        std::ofstream ffdx(segPath("fdx"), std::ios::binary | std::ios::trunc);
        wU32(ffdx, static_cast<uint32_t>(fdx_entries.size()));
        for (const auto& [did, off] : fdx_entries) {
            wU32(ffdx, did);
            wU64(ffdx, off);
        }
    }

    // ── 4. Write .liv (all docs alive) ───────────────────────────────────────
    {
        std::ofstream fliv(segPath("liv"), std::ios::binary | std::ios::trunc);
        wU32(fliv, doc_count_);
        uint32_t bytes = (doc_count_ + 7) / 8;
        std::vector<uint8_t> bitmap(bytes, 0xFF);
        if (doc_count_ % 8 != 0) {
            uint8_t mask = (1u << (doc_count_ % 8)) - 1u;
            bitmap.back() &= mask;
        }
        fliv.write(reinterpret_cast<const char*>(bitmap.data()), bytes);
    }

    // ── 5. Write .si ─────────────────────────────────────────────────────────
    {
        std::ofstream fsi(segPath("si"), std::ios::binary | std::ios::trunc);
        wU32(fsi, seg_id);
        wU32(fsi, doc_count_);
        wU32(fsi, total_term_count);

        time_t now = time(nullptr);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", localtime(&now));
        wStr(fsi, std::string(tbuf));

        std::string fields_str;
        for (size_t i = 0; i < written_fields.size(); ++i) {
            if (i) fields_str += ',';
            fields_str += written_fields[i];
        }
        wStr(fsi, fields_str);
    }

    // ── 6. Write .done, remove .ing ──────────────────────────────────────────
    { std::ofstream done(seg_dir + "/.done"); }
    fs::remove(seg_dir + "/.ing");
}

} // namespace ii::memory
