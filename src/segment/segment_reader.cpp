#include "index/segment_reader.h"
#include "fastfield/fast_field_reader.h"
#include "schema/schema.h"
#include "postings/pfor_delta.h"
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace ii {

// ─────────────────────────────────────────────────────────────────────────────
// 读辅助
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t readU32(std::ifstream& f) {
    uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); return v;
}
static uint64_t readU64(std::ifstream& f) {
    uint64_t v; f.read(reinterpret_cast<char*>(&v), 8); return v;
}
static float readF32(std::ifstream& f) {
    float v; f.read(reinterpret_cast<char*>(&v), 4); return v;
}
static std::string readStr(std::ifstream& f) {
    uint32_t len = readU32(f);
    std::string s(len, '\0');
    f.read(&s[0], len);
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────

SegmentReader::SegmentReader(const std::string& dir, uint32_t segment_id)
    : dir_(dir), seg_id_(segment_id)
{
    loadTim();   // sets indexed_field_names_ if per-field format
    loadFdx();
    loadLiv();
    Schema schema = Schema::load(dir_);
    ff_ = std::make_unique<FastFieldReader>(dir_, seg_id_, doc_count_, schema);

    if (!indexed_field_names_.empty()) {
        // Per-field 模式：为每个字段打开 .doc_<field>、.pos_<field>、.len_<field>
        for (const auto& field : indexed_field_names_) {
            auto doc_p = fieldPath(field, "doc");
            if (std::filesystem::exists(doc_p))
                doc_files_[field].open(doc_p, std::ios::binary);

            // 加载 .len_<field>（per-doc 字段长度）
            auto len_p = fieldPath(field, "len");
            if (std::filesystem::exists(len_p)) {
                std::ifstream fl(len_p, std::ios::binary);
                uint32_t cnt = 0;
                fl.read(reinterpret_cast<char*>(&cnt), 4);
                if (cnt > 0) {
                    field_doc_lens_[field].resize(cnt);
                    fl.read(reinterpret_cast<char*>(field_doc_lens_[field].data()),
                            cnt * sizeof(uint16_t));
                }
            }

            auto pos_p = fieldPath(field, "pos");
            if (std::filesystem::exists(pos_p))
                per_field_pos_files_[field].open(pos_p, std::ios::binary);
        }
        // doc_file_ 指向第一个字段的 .doc（供 legacy readSkipList 使用）
        if (!indexed_field_names_.empty())
            doc_file_.open(fieldPath(indexed_field_names_[0], "doc"), std::ios::binary);
    } else {
        // Legacy 模式：单文件
        doc_file_.open(path("doc"), std::ios::binary);
        pos_file_.open(path("pos"), std::ios::binary);
        if (!doc_file_)
            throw std::runtime_error("Cannot open .doc for segment " + std::to_string(seg_id_));
    }

    fdt_file_.open(path("fdt"), std::ios::binary);
    if (!fdt_file_)
        throw std::runtime_error("Cannot open .fdt for segment " + std::to_string(seg_id_));

    std::cout << "[SegmentReader] Opened segment " << seg_id_
              << ": " << doc_count_ << " docs, "
              << term_dict_.size() << " terms.\n";
}

std::string SegmentReader::path(const std::string& ext) const {
    return dir_ + "/_" + std::to_string(seg_id_) + "." + ext;
}

std::string SegmentReader::fieldPath(const std::string& field,
                                      const std::string& ext) const {
    return dir_ + "/_" + std::to_string(seg_id_) + "." + ext + "_" + field;
}

// ─────────────────────────────────────────────────────────────────────────────
// loadTim：自动检测 per-field 或 legacy 格式
// ─────────────────────────────────────────────────────────────────────────────

void SegmentReader::loadTim() {
    // 先读 .si 获取 doc_count_ 和 indexed_fields（per-field 格式标识）
    std::string indexed_fields;
    {
        std::ifstream fsi(path("si"), std::ios::binary);
        if (fsi) {
            readU32(fsi);  // segment_id
            doc_count_ = readU32(fsi);
            readU32(fsi);  // term_count
            if (fsi.good()) readStr(fsi);  // created_at
            // indexed_fields 仅在 per-field 格式（Step 4+）的 .si 中存在
            if (fsi.good() && fsi.peek() != std::char_traits<char>::eof())
                indexed_fields = readStr(fsi);
        }
    }

    if (!indexed_fields.empty()) {
        loadPerFieldTims(indexed_fields);
        return;
    }

    // Legacy：加载单个 _N.tim
    std::ifstream f(path("tim"), std::ios::binary);
    if (!f) throw std::runtime_error("Cannot open .tim: " + path("tim"));

    uint32_t term_count = readU32(f);
    for (uint32_t i = 0; i < term_count; ++i) {
        std::string term = readStr(f);
        TermMeta meta;
        meta.doc_freq        = readU32(f);
        meta.total_term_freq = readU32(f);
        meta.posting_offset  = readU64(f);
        meta.skip_offset     = readU64(f);
        meta.pos_offset      = readU64(f);
        meta.upper_bound     = readF32(f);
        term_dict_[term] = meta;
        if (doc_count_ == 0)
            doc_count_ = std::max(doc_count_, meta.doc_freq);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// loadPerFieldTims：加载 per-field 格式的词典，合并到 term_dict_
// ─────────────────────────────────────────────────────────────────────────────

void SegmentReader::loadPerFieldTims(const std::string& indexed_fields_str) {
    // 解析逗号分隔的字段名
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= indexed_fields_str.size()) {
        size_t end = indexed_fields_str.find(',', start);
        if (end == std::string::npos) end = indexed_fields_str.size();
        std::string f = indexed_fields_str.substr(start, end - start);
        if (!f.empty()) fields.push_back(f);
        start = end + 1;
    }

    for (const auto& field : fields) {
        std::ifstream f(fieldPath(field, "tim"), std::ios::binary);
        if (!f) continue;
        indexed_field_names_.push_back(field);

        uint32_t term_count = readU32(f);
        uint64_t total_ttf  = 0;
        for (uint32_t i = 0; i < term_count; ++i) {
            std::string term = readStr(f);
            TermMeta meta;
            meta.doc_freq        = readU32(f);
            meta.total_term_freq = readU32(f);
            meta.posting_offset  = readU64(f);
            meta.skip_offset     = readU64(f);
            meta.pos_offset      = readU64(f);
            meta.upper_bound     = readF32(f);
            // tf_data_offset：新格式写入，旧格式文件无此字段（读到 eof 时保持 0）
            meta.tf_data_offset  = (f.peek() != std::char_traits<char>::eof()) ? readU64(f) : 0;
            field_term_dicts_[field][term] = meta;
            total_ttf += meta.total_term_freq;
        }
        // avg_doc_len：总 token 数 / 文档数（Σ TTF / N）
        field_avg_doc_lens_[field] = (doc_count_ > 0)
            ? static_cast<float>(total_ttf) / doc_count_ : 0.f;
    }

    // 合并到 term_dict_（backward-compat 接口）：
    //   ttf = Σ per-field ttf；df = max per-field df（保守近似）
    for (const auto& field : indexed_field_names_) {
        for (const auto& [term, meta] : field_term_dicts_[field]) {
            auto it = term_dict_.find(term);
            if (it == term_dict_.end()) {
                term_dict_[term] = meta;
            } else {
                it->second.total_term_freq += meta.total_term_freq;
                it->second.doc_freq = std::max(it->second.doc_freq, meta.doc_freq);
                it->second.upper_bound = std::max(it->second.upper_bound, meta.upper_bound);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// loadFdx：读取 .fdx 偏移表
// ─────────────────────────────────────────────────────────────────────────────

void SegmentReader::loadFdx() {
    std::ifstream f(path("fdx"), std::ios::binary);
    if (!f) return;  // 允许不存在（空 Segment）

    uint32_t count = readU32(f);
    fdx_offsets_.resize(count + 1, 0);  // index by doc_id (1-based)
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t did = readU32(f);
        uint64_t off = readU64(f);
        if (did < fdx_offsets_.size()) fdx_offsets_[did] = off;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// loadLiv：读取 .liv 位图
// ─────────────────────────────────────────────────────────────────────────────

void SegmentReader::loadLiv() {
    std::ifstream f(path("liv"), std::ios::binary);
    if (!f) {
        // 没有 .liv 文件：全部存活
        liv_bitmap_.assign(doc_count_ + 1, true);
        return;
    }

    uint32_t count = readU32(f);
    uint32_t bytes = (count + 7) / 8;
    std::vector<uint8_t> raw(bytes);
    f.read(reinterpret_cast<char*>(raw.data()), bytes);

    liv_bitmap_.resize(count + 1, false);
    for (uint32_t i = 0; i < count; ++i) {
        liv_bitmap_[i + 1] = (raw[i / 8] >> (i % 8)) & 1u;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// isAlive / softDelete
// ─────────────────────────────────────────────────────────────────────────────

bool SegmentReader::isAlive(DocId doc_id) const {
    if (doc_id >= liv_bitmap_.size()) return false;
    return liv_bitmap_[doc_id];
}

void SegmentReader::softDelete(DocId doc_id) {
    if (doc_id < liv_bitmap_.size()) {
        liv_bitmap_[doc_id] = false;
    }
    // TODO: 持久化到 .liv 文件（此处仅修改内存）
}

// ─────────────────────────────────────────────────────────────────────────────
// readFieldSkipList：从指定字段的 .doc_<field> 读取 SkipList
// ─────────────────────────────────────────────────────────────────────────────

SkipList SegmentReader::readFieldSkipList(const std::string& field,
                                           const TermMeta& meta) const {
    auto it = doc_files_.find(field);
    if (it == doc_files_.end()) return SkipList{};
    auto& f = it->second;
    f.clear();
    f.seekg(static_cast<std::streamoff>(meta.skip_offset));

    uint32_t l0c, l1c;
    f.read(reinterpret_cast<char*>(&l0c), 4);
    f.read(reinterpret_cast<char*>(&l1c), 4);

    size_t total = 8 + (l0c + l1c) * sizeof(SkipNode);
    std::vector<uint8_t> buf(total);
    f.seekg(static_cast<std::streamoff>(meta.skip_offset));
    f.read(reinterpret_cast<char*>(buf.data()), total);
    return SkipList::deserialize(buf.data(), buf.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// getTermMeta（per-field 版本）
// ─────────────────────────────────────────────────────────────────────────────

const TermMeta* SegmentReader::getTermMeta(const std::string& field,
                                            const std::string& term) const {
    auto fit = field_term_dicts_.find(field);
    if (fit == field_term_dicts_.end()) return nullptr;
    auto tit = fit->second.find(term);
    if (tit == fit->second.end()) return nullptr;
    return &tit->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// getTermMeta（legacy，返回合并词典中的 meta）
// ─────────────────────────────────────────────────────────────────────────────

const TermMeta* SegmentReader::getTermMeta(const std::string& term) const {
    auto it = term_dict_.find(term);
    if (it == term_dict_.end()) return nullptr;
    return &it->second;
}

// docFileForTerm：per-field 模式下找到包含该 term 的字段文件句柄
// legacy 模式或找不到时返回 doc_file_（第一个字段或 legacy .doc）
std::ifstream& SegmentReader::docFileForTerm(const std::string& term) const {
    for (auto& [field, dict] : field_term_dicts_) {
        if (dict.count(term)) {
            auto it = doc_files_.find(field);
            if (it != doc_files_.end()) return it->second;
        }
    }
    return doc_file_;
}

// ─────────────────────────────────────────────────────────────────────────────
// readSkipList：从 .doc 文件读取 SkipList（懒加载）
// ─────────────────────────────────────────────────────────────────────────────

SkipList SegmentReader::readSkipList(const TermMeta& meta) const {
    doc_file_.clear();
    doc_file_.seekg(static_cast<std::streamoff>(meta.skip_offset));

    // 读 4B level0_count + 4B level1_count，计算总字节数
    uint32_t l0c, l1c;
    doc_file_.read(reinterpret_cast<char*>(&l0c), 4);
    doc_file_.read(reinterpret_cast<char*>(&l1c), 4);

    size_t total = 8 + (l0c + l1c) * sizeof(SkipNode);
    std::vector<uint8_t> buf(total);

    // 回退，重新读完整序列
    doc_file_.seekg(static_cast<std::streamoff>(meta.skip_offset));
    doc_file_.read(reinterpret_cast<char*>(buf.data()), total);

    return SkipList::deserialize(buf.data(), buf.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// readPostingList：全量解压某 term 的 posting list
// ─────────────────────────────────────────────────────────────────────────────
// Todo: 当前postinglist 是全加载到内存，当postinglist过大时，可能会有性能问题，可以考虑分块加载和解压
std::vector<DocId> SegmentReader::readPostingList(const std::string& term) const {
    // Per-field 模式：找到包含该 term 的字段，通过 postingIterator 读取
    // （每次创建独立文件句柄，避免共享 ifstream 的 seek 状态污染）
    if (!field_term_dicts_.empty()) {
        for (const auto& [field, dict] : field_term_dicts_) {
            if (dict.count(term)) {
                PostingIterator iter = postingIterator(field, term);
                std::vector<DocId> result;
                const TermMeta* m = getTermMeta(field, term);
                if (m) result.reserve(m->doc_freq);
                while (!iter.isEnd()) {
                    result.push_back(iter.docId());
                    iter.next();
                }
                return result;
            }
        }
        return {};
    }

    // Legacy 模式：直接从 doc_file_ 读取
    const TermMeta* meta = getTermMeta(term);
    if (!meta) return {};

    doc_file_.clear();
    doc_file_.seekg(static_cast<std::streamoff>(meta->posting_offset));

    uint32_t remaining = meta->doc_freq;
    std::vector<DocId> result;
    result.reserve(remaining);

    while (remaining > 0) {
        BlockHeader hdr;
        doc_file_.read(reinterpret_cast<char*>(&hdr), sizeof(BlockHeader));
        if (!doc_file_) break;

        size_t main_bits  = (size_t)hdr.size * hdr.b;
        size_t main_bytes = (main_bits + 7) / 8;
        size_t patch_bytes= (size_t)hdr.exc_count * sizeof(PatchEntry);
        size_t block_body = main_bytes + patch_bytes;

        std::vector<uint8_t> block_data(sizeof(BlockHeader) + block_body);
        std::memcpy(block_data.data(), &hdr, sizeof(BlockHeader));
        doc_file_.read(reinterpret_cast<char*>(block_data.data() + sizeof(BlockHeader)),
                       block_body);

        PForDelta::decompressBlock(block_data.data(), result);
        remaining -= hdr.size;
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// readPostingListFrom：只返回 >= target_doc_id 的部分（利用 SkipList 跳跃）
// ─────────────────────────────────────────────────────────────────────────────

std::vector<DocId> SegmentReader::readPostingListFrom(
    const std::string& term, DocId target_doc_id) const
{
    const TermMeta* meta = getTermMeta(term);
    if (!meta) return {};

    std::ifstream& f = docFileForTerm(term);

    // SkipList 也必须从同一字段文件中读取
    f.clear();
    f.seekg(static_cast<std::streamoff>(meta->skip_offset));
    uint32_t l0c, l1c;
    f.read(reinterpret_cast<char*>(&l0c), 4);
    f.read(reinterpret_cast<char*>(&l1c), 4);
    size_t total_sl = 8 + (l0c + l1c) * sizeof(SkipNode);
    std::vector<uint8_t> sl_buf(total_sl);
    f.seekg(static_cast<std::streamoff>(meta->skip_offset));
    f.read(reinterpret_cast<char*>(sl_buf.data()), total_sl);
    SkipList sl = SkipList::deserialize(sl_buf.data(), sl_buf.size());

    auto found = sl.find(target_doc_id);
    uint64_t start_offset = (found.byte_offset == UINT64_MAX)
                             ? meta->posting_offset
                             : meta->posting_offset + found.byte_offset;

    f.clear();
    f.seekg(static_cast<std::streamoff>(start_offset));

    uint32_t remaining = meta->doc_freq;
    if (found.block_index != SIZE_MAX) {
        for (size_t i = 0; i < found.block_index && remaining > 0; ++i)
            remaining -= std::min(remaining, 128u);
    }

    std::vector<DocId> result;
    while (remaining > 0) {
        BlockHeader hdr;
        f.read(reinterpret_cast<char*>(&hdr), sizeof(BlockHeader));
        if (!f) break;

        size_t main_bytes = ((size_t)hdr.size * hdr.b + 7) / 8;
        size_t patch_bytes= (size_t)hdr.exc_count * sizeof(PatchEntry);
        size_t block_body = main_bytes + patch_bytes;

        std::vector<uint8_t> block_data(sizeof(BlockHeader) + block_body);
        std::memcpy(block_data.data(), &hdr, sizeof(BlockHeader));
        f.read(reinterpret_cast<char*>(block_data.data() + sizeof(BlockHeader)), block_body);

        std::vector<DocId> block_docs;
        PForDelta::decompressBlock(block_data.data(), block_docs);

        for (DocId d : block_docs) {
            if (d >= target_doc_id) result.push_back(d);
        }
        remaining -= hdr.size;
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// readPosEntries：从 .pos 读取某 term 的全部 <doc_id, tf, positions>
// ─────────────────────────────────────────────────────────────────────────────

std::vector<PostingEntry> SegmentReader::readPosEntries(const std::string& term) const {
    if (!indexed_field_names_.empty()) {
        // Per-field 模式：从各字段 .pos_<field> 读取，按 doc_id 合并
        std::map<DocId, PostingEntry> merged;
        for (const auto& field : indexed_field_names_) {
            auto fit = field_term_dicts_.find(field);
            if (fit == field_term_dicts_.end()) continue;
            auto tit = fit->second.find(term);
            if (tit == fit->second.end()) continue;

            const TermMeta& fmeta = tit->second;
            auto pfit = per_field_pos_files_.find(field);
            if (pfit == per_field_pos_files_.end() || !pfit->second) continue;

            auto& pf = pfit->second;
            pf.clear();
            pf.seekg(static_cast<std::streamoff>(fmeta.pos_offset));

            for (uint32_t i = 0; i < fmeta.doc_freq; ++i) {
                DocId   doc_id; uint32_t tf;
                pf.read(reinterpret_cast<char*>(&doc_id), 4);
                pf.read(reinterpret_cast<char*>(&tf),     4);
                if (!pf) break;

                auto& e = merged[doc_id];
                e.doc_id = doc_id;
                e.tf    += tf;
                for (uint32_t j = 0; j < tf; ++j) {
                    Pos p; pf.read(reinterpret_cast<char*>(&p), 4);
                    e.positions.push_back(p);
                }
            }
        }
        std::vector<PostingEntry> result;
        result.reserve(merged.size());
        for (auto& [did, entry] : merged) {
            std::sort(entry.positions.begin(), entry.positions.end());
            result.push_back(std::move(entry));
        }
        return result;
    }

    // Legacy 模式
    const TermMeta* meta = getTermMeta(term);
    if (!meta) return {};

    pos_file_.clear();
    pos_file_.seekg(static_cast<std::streamoff>(meta->pos_offset));
    if (!pos_file_) return {};

    std::vector<PostingEntry> result;
    result.reserve(meta->doc_freq);
    for (uint32_t i = 0; i < meta->doc_freq; ++i) {
        PostingEntry entry;
        pos_file_.read(reinterpret_cast<char*>(&entry.doc_id), 4);
        pos_file_.read(reinterpret_cast<char*>(&entry.tf), 4);
        if (!pos_file_) break;

        entry.positions.resize(entry.tf);
        for (uint32_t j = 0; j < entry.tf; ++j)
            pos_file_.read(reinterpret_cast<char*>(&entry.positions[j]), 4);

        result.push_back(std::move(entry));
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// readStoredDoc：从 .fdt 读取文档原文
// ─────────────────────────────────────────────────────────────────────────────

SegmentReader::StoredDocResult SegmentReader::readStoredDoc(DocId doc_id) const {
    StoredDocResult r;
    if (doc_id == 0 || doc_id >= fdx_offsets_.size()) return r;

    uint64_t off = fdx_offsets_[doc_id];
    fdt_file_.seekg(static_cast<std::streamoff>(off));

    uint32_t did = 0;
    fdt_file_.read(reinterpret_cast<char*>(&did), 4);
    r.doc_id = did;

    uint64_t eid = 0;
    fdt_file_.read(reinterpret_cast<char*>(&eid), 8);
    r.ext_id = eid;

    auto readStr = [&]() -> std::string {
        uint32_t len = 0;
        fdt_file_.read(reinterpret_cast<char*>(&len), 4);
        std::string s(len, '\0');
        fdt_file_.read(&s[0], len);
        return s;
    };

    uint32_t field_count = 0;
    fdt_file_.read(reinterpret_cast<char*>(&field_count), 4);
    for (uint32_t i = 0; i < field_count; ++i) {
        std::string name  = readStr();
        std::string value = readStr();
        r.str_fields[std::move(name)] = std::move(value);
    }
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// bm25Score（per-field）：字段级 BM25，使用 per-field avg_doc_len
// ─────────────────────────────────────────────────────────────────────────────

float SegmentReader::bm25Score(
    DocId doc_id,
    const std::string& field,
    const std::vector<std::string>& query_terms,
    const std::unordered_map<std::string, float>& term_idfs) const
{
    const float k1 = 1.2f, b = 0.75f;
    float avgdl = fieldAvgDocLen(field);
    float dl    = static_cast<float>(fieldDocLen(field, doc_id));
    // avgdl=0 表示旧索引无 .len 文件，退化为 b=0
    float len_factor = (avgdl > 0.f) ? (1.0f - b + b * dl / avgdl) : 1.0f;

    float score = 0.0f;
    for (const auto& term : query_terms) {
        if (!getTermMeta(field, term)) continue;

        float idf = 0.f;
        auto idf_it = term_idfs.find(field + ":" + term);
        if (idf_it != term_idfs.end()) {
            idf = idf_it->second;
        } else {
            idf_it = term_idfs.find(term);
            if (idf_it != term_idfs.end()) idf = idf_it->second;
        }
        if (idf == 0.f) continue;

        auto iter = postingIterator(field, term);
        if (!iter.advance(doc_id) || iter.docId() != doc_id) continue;

        float tf_f    = static_cast<float>(iter.tf());
        float tf_norm = tf_f * (k1 + 1.0f) / (tf_f + k1 * len_factor);
        score += tf_norm * idf;
    }
    return score;
}

// ─────────────────────────────────────────────────────────────────────────────
// bm25Score（legacy）：跨字段合并，不感知字段维度
// ─────────────────────────────────────────────────────────────────────────────

float SegmentReader::bm25Score(
    DocId doc_id,
    const std::vector<std::string>& query_terms,
    const std::unordered_map<std::string, float>& term_idfs) const
{
    const float k1 = 1.2f;  // legacy 路径无字段信息，保持 b=0 近似
    float score = 0.0f;

    for (const auto& term : query_terms) {
        if (!getTermMeta(term)) continue;

        auto idf_it = term_idfs.find(term);
        if (idf_it == term_idfs.end()) continue;
        float idf = idf_it->second;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        auto iter = postingIterator(term);
#pragma clang diagnostic pop
        if (!iter.advance(doc_id) || iter.docId() != doc_id) continue;

        float tf_f    = static_cast<float>(iter.tf());
        float tf_norm = tf_f * (k1 + 1.0f) / (tf_f + k1);
        score += tf_norm * idf;
    }
    return score;
}

// ─────────────────────────────────────────────────────────────────────────────
// FastField 接口
// ─────────────────────────────────────────────────────────────────────────────

int64_t SegmentReader::ffPubtime(uint32_t local_doc_idx) const {
    return ff_ ? ff_->pubtime(local_doc_idx) : 0;
}

int64_t SegmentReader::ffUid(uint32_t local_doc_idx) const {
    return ff_ ? ff_->uid(local_doc_idx) : 0;
}

float SegmentReader::ffPageRank(uint32_t local_doc_idx) const {
    return ff_ ? ff_->pageRank(local_doc_idx) : 0.0f;
}

std::vector<uint32_t> SegmentReader::filterPubtime(int64_t lo, int64_t hi) const {
    return ff_ ? ff_->filterPubtime(lo, hi) : std::vector<uint32_t>{};
}

std::vector<uint32_t> SegmentReader::filterUid(int64_t uid_val) const {
    return ff_ ? ff_->filterUid(uid_val) : std::vector<uint32_t>{};
}

bool SegmentReader::hasFastField() const {
    return ff_ && ff_->hasData();
}

// ─────────────────────────────────────────────────────────────────────────────
// postingIterator（per-field）：每次调用打开独立文件句柄
// ─────────────────────────────────────────────────────────────────────────────

PostingIterator SegmentReader::postingIterator(const std::string& field,
                                                const std::string& term) const {
    const TermMeta* meta = getTermMeta(field, term);
    if (!meta) return PostingIterator();
    return PostingIterator(*meta, fieldPath(field, "doc"));
}

// ─────────────────────────────────────────────────────────────────────────────
// postingIterator（legacy）：返回第一个命中字段的迭代器
// ─────────────────────────────────────────────────────────────────────────────

PostingIterator SegmentReader::postingIterator(const std::string& term) const {
    if (!indexed_field_names_.empty()) {
        for (const auto& field : indexed_field_names_) {
            auto fit = field_term_dicts_.find(field);
            if (fit == field_term_dicts_.end()) continue;
            if (fit->second.count(term))
                return PostingIterator(fit->second.at(term), fieldPath(field, "doc"));
        }
        return PostingIterator();
    }
    const TermMeta* meta = getTermMeta(term);
    if (!meta) return PostingIterator();
    return PostingIterator(*meta, path("doc"));
}

// ─────────────────────────────────────────────────────────────────────────────
// readPosEntries（per-field）：从 .pos_<field> 读取 <doc_id, tf, positions>
// ─────────────────────────────────────────────────────────────────────────────

std::vector<PostingEntry> SegmentReader::readPosEntries(const std::string& field,
                                                         const std::string& term) const {
    const TermMeta* meta = getTermMeta(field, term);
    if (!meta) return {};

    auto pfit = per_field_pos_files_.find(field);
    if (pfit == per_field_pos_files_.end() || !pfit->second) return {};

    auto& pf = pfit->second;
    pf.clear();
    pf.seekg(static_cast<std::streamoff>(meta->pos_offset));

    std::vector<PostingEntry> result;
    result.reserve(meta->doc_freq);
    for (uint32_t i = 0; i < meta->doc_freq; ++i) {
        PostingEntry entry;
        pf.read(reinterpret_cast<char*>(&entry.doc_id), 4);
        pf.read(reinterpret_cast<char*>(&entry.tf),     4);
        if (!pf) break;
        entry.positions.resize(entry.tf);
        for (uint32_t j = 0; j < entry.tf; ++j)
            pf.read(reinterpret_cast<char*>(&entry.positions[j]), 4);
        result.push_back(std::move(entry));
    }
    return result;
}

} // namespace ii
