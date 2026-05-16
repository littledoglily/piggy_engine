#include "segment/segment_reader.h"
#include "fastfield/fast_field_reader.h"
#include "schema/schema.h"
#include "postings/pfor_delta.h"
#include <cstring>
#include <cmath>
#include <stdexcept>
#include <algorithm>
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
    loadTim();
    loadFdx();
    loadLiv();
    Schema schema = Schema::load(dir_);
    ff_ = std::make_unique<FastFieldReader>(dir_, seg_id_, doc_count_, schema);

    // 打开磁盘文件（保持 ifstream 打开，按需 seek）
    doc_file_.open(dir_ + "/_" + std::to_string(seg_id_) + ".doc",
                   std::ios::binary);
    pos_file_.open(dir_ + "/_" + std::to_string(seg_id_) + ".pos",
                   std::ios::binary);
    fdt_file_.open(dir_ + "/_" + std::to_string(seg_id_) + ".fdt",
                   std::ios::binary);

    if (!doc_file_) throw std::runtime_error("Cannot open .doc for segment " + std::to_string(seg_id_));
    if (!fdt_file_) throw std::runtime_error("Cannot open .fdt for segment " + std::to_string(seg_id_));

    std::cout << "[SegmentReader] Opened segment " << seg_id_
              << ": " << doc_count_ << " docs, "
              << term_dict_.size() << " terms.\n";
}

std::string SegmentReader::path(const std::string& ext) const {
    return dir_ + "/_" + std::to_string(seg_id_) + "." + ext;
}

// ─────────────────────────────────────────────────────────────────────────────
// loadTim：读取 .tim 词典，全部加载到 term_dict_（模拟 JVM 堆常驻）
// ─────────────────────────────────────────────────────────────────────────────

void SegmentReader::loadTim() {
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
        doc_count_ = std::max(doc_count_, meta.doc_freq);
    }

    // 读 .si 获得 doc_count 精确值
    std::ifstream fsi(path("si"), std::ios::binary);
    if (fsi) {
        readU32(fsi);  // segment_id
        doc_count_ = readU32(fsi);
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
// getTermMeta
// ─────────────────────────────────────────────────────────────────────────────

const TermMeta* SegmentReader::getTermMeta(const std::string& term) const {
    auto it = term_dict_.find(term);
    if (it == term_dict_.end()) return nullptr;
    return &it->second;
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
    const TermMeta* meta = getTermMeta(term);
    if (!meta) return {};

    doc_file_.clear();
    doc_file_.seekg(static_cast<std::streamoff>(meta->posting_offset));

    // 估计总字节数：读到文件末尾或下一个 term 的 skip_offset
    // 简化：读 doc_freq 个 doc，按 Block 解压
    uint32_t remaining = meta->doc_freq;
    std::vector<DocId> result;
    result.reserve(remaining);

    while (remaining > 0) {
        // 读一个 Block 的 Header（16 byte）
        BlockHeader hdr;
        doc_file_.read(reinterpret_cast<char*>(&hdr), sizeof(BlockHeader));
        if (!doc_file_) break;

        // 计算该 Block 的剩余字节数
        size_t main_bits  = (size_t)hdr.size * hdr.b;
        size_t main_bytes = (main_bits + 7) / 8;
        size_t patch_bytes= (size_t)hdr.exc_count * sizeof(PatchEntry);
        size_t block_body = main_bytes + patch_bytes;

        // 读 Block 完整数据（Header + 主数据区 + 补丁区）
        std::vector<uint8_t> block_data(sizeof(BlockHeader) + block_body);
        std::memcpy(block_data.data(), &hdr, sizeof(BlockHeader));
        doc_file_.read(reinterpret_cast<char*>(block_data.data() + sizeof(BlockHeader)),
                       block_body);

        // 解压
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

    // 用 SkipList 定位目标 Block
    SkipList sl = readSkipList(*meta);
    auto found  = sl.find(target_doc_id);

    uint64_t start_offset = (found.byte_offset == UINT64_MAX)
                             ? meta->posting_offset
                             : meta->posting_offset + found.byte_offset;

    doc_file_.seekg(static_cast<std::streamoff>(start_offset));

    uint32_t remaining = meta->doc_freq;
    // 跳过已通过的 Block
    if (found.block_index != SIZE_MAX) {
        for (size_t i = 0; i < found.block_index && remaining > 0; ++i) {
            remaining -= std::min(remaining, 128u);
        }
    }

    std::vector<DocId> result;
    while (remaining > 0) {
        BlockHeader hdr;
        doc_file_.read(reinterpret_cast<char*>(&hdr), sizeof(BlockHeader));
        if (!doc_file_) break;

        size_t main_bytes = ((size_t)hdr.size * hdr.b + 7) / 8;
        size_t patch_bytes= (size_t)hdr.exc_count * sizeof(PatchEntry);
        size_t block_body = main_bytes + patch_bytes;

        std::vector<uint8_t> block_data(sizeof(BlockHeader) + block_body);
        std::memcpy(block_data.data(), &hdr, sizeof(BlockHeader));
        doc_file_.read(reinterpret_cast<char*>(block_data.data() + sizeof(BlockHeader)),
                       block_body);

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
    const TermMeta* meta = getTermMeta(term);
    if (!meta) return {};

    pos_file_.clear();  // seekg 不会自动清除 eofbit，必须先 clear 再检查
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

    r.source   = readStr();
    r.title    = readStr();
    r.body     = readStr();
    r.category = readStr();
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// bm25Score：BM25 打分（简化，不考虑 doc length）
// ─────────────────────────────────────────────────────────────────────────────

float SegmentReader::bm25Score(
    DocId doc_id,
    const std::vector<std::string>& query_terms,
    const std::unordered_map<std::string, float>& term_idfs) const
{
    const float k1 = 1.2f;
    float score = 0.0f;

    for (const auto& term : query_terms) {
        const TermMeta* meta = getTermMeta(term);
        if (!meta) continue;

        auto idf_it = term_idfs.find(term);
        if (idf_it == term_idfs.end()) continue;
        float idf = idf_it->second;

        // tf：全量读 posting list 查找该 doc（生产应缓存）
        auto pl = readPostingList(term);
        uint32_t tf = 0;
        for (DocId d : pl) {
            if (d == doc_id) { tf = 1; break; }  // 简化 tf=1
        }
        if (tf == 0) continue;

        // tf_norm：dl=avgdl 简化（k1=1.2）
        float tf_norm = (float)tf * (k1 + 1.0f) / ((float)tf + k1);
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

} // namespace ii
