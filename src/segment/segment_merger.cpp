#include "segment/segment_merger.h"
#include "fastfield/fast_field_writer.h"
#include "schema/schema.h"
#include <cmath>
#include "postings/pfor_delta.h"
#include "postings/skiplist.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace ii {

// ── 写辅助（与 segment_writer.cpp 一致，局部复用）────────────────────────────
static void wU32(std::ofstream& f, uint32_t v) { f.write((char*)&v, 4); }
static void wU64(std::ofstream& f, uint64_t v) { f.write((char*)&v, 8); }
static void wF32(std::ofstream& f, float    v) { f.write((char*)&v, 4); }
static void wStr(std::ofstream& f, const std::string& s) {
    uint32_t l = s.size(); wU32(f, l); f.write(s.data(), l);
}
static void wBytes(std::ofstream& f, const std::vector<uint8_t>& b) {
    f.write((char*)b.data(), b.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// 构造：扫描目录，加载所有活跃 Segment
// ─────────────────────────────────────────────────────────────────────────────

SegmentMerger::SegmentMerger(const std::string& dir) : dir_(dir) {
    // 扫描 .si 文件得到所有 Segment ID
    for (const auto& e : std::filesystem::directory_iterator(dir_)) {
        auto name = e.path().filename().string();
        if (name.size() > 4 && name[0] == '_' &&
            name.substr(name.size()-3) == ".si") {
            try {
                uint32_t id = std::stoul(name.substr(1, name.size()-4));
                seg_ids_.push_back(id);
            } catch (...) {}
        }
    }
    std::sort(seg_ids_.begin(), seg_ids_.end());

    for (uint32_t id : seg_ids_) {
        readers_.push_back(std::make_unique<SegmentReader>(dir_, id));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 软删除：在所有 Segment 中找到 doc_id 并在 .liv 中标记
// ─────────────────────────────────────────────────────────────────────────────
// TODO：软删除并没有并发控制, 所以只是toy版本，需要保证并发读写问题；
bool SegmentMerger::softDelete(DocId doc_id) {
    for (auto& reader : readers_) {
        if (reader->isAlive(doc_id)) {
            reader->softDelete(doc_id);

            // 持久化 .liv 文件
            // 读取现有 .liv，修改对应 bit，写回
            std::string liv_path = segPath(reader->segmentId(), "liv");
            std::fstream f(liv_path, std::ios::binary | std::ios::in | std::ios::out);
            if (!f) return true;  // 内存已更新，文件操作失败不影响功能

            uint32_t count;
            f.read((char*)&count, 4);
            uint32_t bytes = (count + 7) / 8;
            std::vector<uint8_t> bitmap(bytes);
            f.read((char*)bitmap.data(), bytes);

            // doc_id 是 1-indexed，bit 下标 = doc_id - 1
            if (doc_id > 0 && doc_id <= count) {
                uint32_t bit_idx = doc_id - 1;
                bitmap[bit_idx / 8] &= ~(1u << (bit_idx % 8));  // 清零

                // 写回
                f.seekp(4);
                f.write((char*)bitmap.data(), bytes);
            }
            std::cout << "[SoftDelete] DocID=" << doc_id
                      << " marked deleted in segment " << reader->segmentId() << "\n";
            return true;
        }
    }
    std::cout << "[SoftDelete] DocID=" << doc_id << " not found.\n";
    return false;
}

int SegmentMerger::softDeleteBatch(const std::vector<DocId>& doc_ids) {
    int count = 0;
    for (DocId id : doc_ids) if (softDelete(id)) ++count;
    return count;
}

bool SegmentMerger::isAlive(DocId doc_id) const {
    for (const auto& r : readers_) {
        if (r->isAlive(doc_id)) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// mergeIfNeeded：按策略判断是否触发合并
// ─────────────────────────────────────────────────────────────────────────────

MergeStats SegmentMerger::mergeIfNeeded(MergePolicy policy, int max_segments) {
    if (policy == MergePolicy::FORCE) {
        uint32_t new_id = *std::max_element(seg_ids_.begin(), seg_ids_.end()) + 1;
        return doMerge(seg_ids_, new_id);
    }

    // TIERED：超过阈值才合并
    if ((int)seg_ids_.size() <= max_segments) {
        std::cout << "[Merger] Segment count=" << seg_ids_.size()
                  << " <= threshold=" << max_segments << ", skip merge.\n";
        MergeStats s{};
        return s;
    }

    uint32_t new_id = *std::max_element(seg_ids_.begin(), seg_ids_.end()) + 1;
    return doMerge(seg_ids_, new_id);
}

MergeStats SegmentMerger::mergeAll(uint32_t new_segment_id) {
    return doMerge(seg_ids_, new_segment_id);
}

// ─────────────────────────────────────────────────────────────────────────────
// doMerge：核心合并逻辑
// ─────────────────────────────────────────────────────────────────────────────

MergeStats SegmentMerger::doMerge(const std::vector<uint32_t>& src_ids,
                                   uint32_t new_segment_id)
{
    std::cout << "\n[Merger] ══════════════════════════════════════════\n";
    std::cout << "[Merger] Starting merge: segments [";
    for (size_t i = 0; i < src_ids.size(); ++i) {
        std::cout << src_ids[i]; if (i+1<src_ids.size()) std::cout << ",";
    }
    std::cout << "] → segment " << new_segment_id << "\n";

    // ── Step1: 统计存活文档，建立全局 doc_id 重映射表 ─────────────────────────
    // 格式：seg_id × old_local_doc_id → new_global_doc_id
    // 重映射去掉被软删除的 doc，重新从 1 连续编号
    struct GlobalDoc {
        uint32_t    seg_id;
        DocId       orig_doc_id;
        uint64_t    ext_id = 0;
        std::unordered_map<std::string, std::string> str_fields;
    };
    std::vector<GlobalDoc> alive_docs;

    uint32_t total_input  = 0;
    uint32_t total_deleted= 0;

    // 为每个 (seg_id, local_doc_id) 分配新 global_doc_id
    // key = (seg_id << 20) | local_doc_id  (假设 local_doc_id < 2^20)
    std::unordered_map<uint64_t, DocId> remap;

    DocId new_id = 1;
    for (uint32_t sid : src_ids) {
        // 找到对应 reader
        SegmentReader* reader = nullptr;
        for (auto& r : readers_) {
            if (r->segmentId() == sid) { reader = r.get(); break; }
        }
        if (!reader) continue;

        uint32_t dc = reader->docCount();
        total_input += dc;

        for (uint32_t local = 1; local <= dc; ++local) {
            if (!reader->isAlive(local)) {
                ++total_deleted;
                continue;
            }
            // 存活：用 .fdt 中存储的原始 doc_id 作为 key，与 .pos 保持一致
            auto stored = reader->readStoredDoc(local);
            uint64_t key = ((uint64_t)sid << 20) | stored.doc_id;
            remap[key] = new_id;
            GlobalDoc gd;
            gd.seg_id      = sid;
            gd.orig_doc_id = stored.doc_id;
            gd.ext_id      = stored.ext_id;
            gd.str_fields  = stored.str_fields;
            alive_docs.push_back(std::move(gd));
            ++new_id;
        }
    }
    uint32_t output_doc_count = new_id - 1;

    std::cout << "[Merger] Input docs=" << total_input
              << " deleted=" << total_deleted
              << " alive=" << output_doc_count << "\n";

    // ── Step2: 建立合并词表（所有 Segment 的 term 并集）─────────────────────
    std::set<std::string> all_terms;
    for (uint32_t sid : src_ids) {
        SegmentReader* reader = nullptr;
        for (auto& r : readers_) {
            if (r->segmentId() == sid) { reader = r.get(); break; }
        }
        if (!reader) continue;

        // 读 .tim 词典，获取所有 term
        std::ifstream ftim(segPath(sid, "tim"), std::ios::binary);
        if (!ftim) continue;
        uint32_t tc; ftim.read((char*)&tc, 4);
        for (uint32_t i = 0; i < tc; ++i) {
            uint32_t len; ftim.read((char*)&len, 4);
            std::string term(len, '\0'); ftim.read(&term[0], len);
            all_terms.insert(term);
            // skip rest of entry (4+4+8+8+8+4 = 36 bytes)
            ftim.seekg(36, std::ios::cur);
        }
    }
    std::cout << "[Merger] Merged term count=" << all_terms.size() << "\n";

    // ── Step3: 对每个 term 多路归并 posting list ──────────────────────────────
    // 结果写入新 Segment 的 .doc / .tim / .pos 文件

    // 先准备新 .tim 的数据结构
    struct NewTermMeta {
        uint32_t df;
        uint32_t total_tf;
        uint64_t posting_offset;
        uint64_t skip_offset;
        uint64_t pos_offset;
        float    upper_bound;
    };
    std::map<std::string, NewTermMeta> new_term_dict;

    // 打开输出文件
    std::ofstream fdoc(segPath(new_segment_id, "doc"), std::ios::binary|std::ios::trunc);
    std::ofstream fpos(segPath(new_segment_id, "pos"), std::ios::binary|std::ios::trunc);
    if (!fdoc || !fpos) throw std::runtime_error("Cannot create merge output files");

    // BM25 参数
    const float k1 = 1.2f, b_param = 0.75f;
    float avg_doc_len = 70.0f;  // 近似值

    for (const auto& term : all_terms) {
        // 收集该 term 在所有源 Segment 中的存活 posting entries
        // 用新 global_doc_id，按升序排列
        std::vector<std::pair<DocId, uint32_t>> merged;  // <new_doc_id, tf>
        std::vector<std::pair<DocId, std::vector<Pos>>> merged_pos;  // <new_doc_id, positions>

        for (uint32_t sid : src_ids) {
            SegmentReader* reader = nullptr;
            for (auto& r : readers_) {
                if (r->segmentId() == sid) { reader = r.get(); break; }
            }
            if (!reader) continue;

            const TermMeta* tm = reader->getTermMeta(term);
            if (!tm) continue;

            // 从 .pos 读取该 term 的真实 tf 和位置信息
            auto pos_entries = reader->readPosEntries(term);

            for (const auto& entry : pos_entries) {
                // remap 已在 step1 过滤死文档，查不到即跳过
                uint64_t key = ((uint64_t)sid << 20) | entry.doc_id;
                auto it = remap.find(key);
                if (it == remap.end()) continue;
                DocId new_doc_id = it->second;

                merged.push_back({new_doc_id, entry.tf});
                merged_pos.push_back({new_doc_id, entry.positions});
            }
        }

        if (merged.empty()) continue;

        // 按新 doc_id 排序（多 Segment 归并后可能乱序）
        std::sort(merged.begin(), merged.end());
        std::sort(merged_pos.begin(), merged_pos.end());

        // 计算 IDF / UB
        uint32_t df   = (uint32_t)merged.size();
        float    idf  = std::log(1.0f + (float)(output_doc_count - df + 0.5f)
                                       / (float)(df + 0.5f));
        uint32_t max_tf = 0;
        for (auto& [did, tf] : merged) max_tf = std::max(max_tf, tf);
        float max_tf_f = static_cast<float>(max_tf > 0 ? max_tf : 1);
        float    ub   = (max_tf_f * (k1+1.0f) / (max_tf_f + k1*(1.0f-b_param+b_param))) * idf;

        // 提取 doc_id 列表
        std::vector<DocId> new_doc_ids;
        new_doc_ids.reserve(merged.size());
        for (auto& [did, tf] : merged) new_doc_ids.push_back(did);

        // 计算每 Block 的 max_tf_norm（不含 IDF）
        constexpr int BLOCK_SZ = 128;
        std::vector<float> block_ubs;
        block_ubs.reserve((merged.size() + BLOCK_SZ - 1) / BLOCK_SZ);
        for (size_t bi = 0; bi < merged.size(); bi += BLOCK_SZ) {
            size_t bend = std::min(bi + (size_t)BLOCK_SZ, merged.size());
            float blk_max = 0.0f;
            for (size_t j = bi; j < bend; ++j) {
                float tf_f = static_cast<float>(merged[j].second);
                float norm = tf_f * (k1 + 1.0f) / (tf_f + k1);
                blk_max = std::max(blk_max, norm);
            }
            block_ubs.push_back(blk_max);
        }

        // PForDelta 压缩
        std::vector<SkipNode> skip_nodes;
        auto compressed = PForDelta::compress(new_doc_ids, skip_nodes, block_ubs);

        // SkipList 序列化
        SkipList sl(skip_nodes);
        auto sl_bytes = sl.serialize();

        // 记录偏移
        NewTermMeta ntm;
        ntm.df             = df;
        uint32_t total_tf = 0;
        for (auto& [did, tf] : merged) total_tf += tf;
        ntm.total_tf       = total_tf;
        ntm.skip_offset    = (uint64_t)fdoc.tellp();
        ntm.posting_offset = ntm.skip_offset + sl_bytes.size();
        ntm.pos_offset     = (uint64_t)fpos.tellp();
        ntm.upper_bound    = ub;

        // 写 .doc
        wBytes(fdoc, sl_bytes);
        wBytes(fdoc, compressed);

        // 写 .pos
        for (auto& [did, positions] : merged_pos) {
            wU32(fpos, did);
            wU32(fpos, (uint32_t)positions.size());
            for (Pos p : positions) wU32(fpos, p);
        }

        new_term_dict[term] = ntm;
    }
    fdoc.close();
    fpos.close();

    // ── Step4: 写新 .tim ─────────────────────────────────────────────────────
    {
        std::ofstream ftim(segPath(new_segment_id, "tim"),
                           std::ios::binary|std::ios::trunc);
        wU32(ftim, (uint32_t)new_term_dict.size());
        for (const auto& [term, m] : new_term_dict) {
            wStr(ftim, term);
            wU32(ftim, m.df);
            wU32(ftim, m.total_tf);
            wU64(ftim, m.posting_offset);
            wU64(ftim, m.skip_offset);
            wU64(ftim, m.pos_offset);
            wF32(ftim, m.upper_bound);
        }
    }

    // ── Step5: 写新 .fdt / .fdx（合并后的文档原文）───────────────────────────
    std::vector<uint64_t> fdx_offsets;
    {
        std::ofstream ffdt(segPath(new_segment_id, "fdt"),
                           std::ios::binary|std::ios::trunc);
        for (const auto& gd : alive_docs) {
            fdx_offsets.push_back((uint64_t)ffdt.tellp());
            DocId new_doc_id = remap[((uint64_t)gd.seg_id << 20) | gd.orig_doc_id];
            wU32(ffdt, new_doc_id);
            wU64(ffdt, gd.ext_id);
            wU32(ffdt, (uint32_t)gd.str_fields.size());
            for (const auto& [name, val] : gd.str_fields) {
                wStr(ffdt, name);
                wStr(ffdt, val);
            }
        }
    }
    {
        std::ofstream ffdx(segPath(new_segment_id, "fdx"),
                           std::ios::binary|std::ios::trunc);
        wU32(ffdx, (uint32_t)fdx_offsets.size());
        for (size_t i = 0; i < fdx_offsets.size(); ++i) {
            wU32(ffdx, (uint32_t)(i+1));
            wU64(ffdx, fdx_offsets[i]);
        }
    }

    // ── Step6: 写新 .liv（全部存活）──────────────────────────────────────────
    {
        std::ofstream fliv(segPath(new_segment_id, "liv"),
                           std::ios::binary|std::ios::trunc);
        wU32(fliv, output_doc_count);
        uint32_t bytes = (output_doc_count + 7) / 8;
        std::vector<uint8_t> bitmap(bytes, 0xFF);
        if (output_doc_count % 8 != 0)
            bitmap.back() &= (1u << (output_doc_count % 8)) - 1u;
        fliv.write((char*)bitmap.data(), bytes);
    }

    // ── Step7: 写新 .si ───────────────────────────────────────────────────────
    {
        std::ofstream fsi(segPath(new_segment_id, "si"),
                          std::ios::binary|std::ios::trunc);
        wU32(fsi, new_segment_id);
        wU32(fsi, output_doc_count);
        wU32(fsi, (uint32_t)new_term_dict.size());
        std::string ts = "merged";
        wStr(fsi, ts);
    }

    // ── Step8: 重建 FastField 列存（按 Schema 驱动，支持任意字段名）────────────
    {
        Schema schema = Schema::load(dir_);
        FastFieldWriter ff_writer;
        for (const auto& gd : alive_docs) {
            SegmentReader* reader = nullptr;
            for (auto& r : readers_) {
                if (r->segmentId() == gd.seg_id) { reader = r.get(); break; }
            }
            uint32_t idx = gd.orig_doc_id - 1;  // 0-indexed
            for (const auto* fs : schema.fastFields()) {
                if (!reader || !reader->hasFastField()) {
                    if (fs->type == FieldType::Int64)   ff_writer.addInt64  (fs->name, 0);
                    if (fs->type == FieldType::Float32) ff_writer.addFloat32(fs->name, 0.0f);
                } else if (fs->type == FieldType::Int64) {
                    ff_writer.addInt64(fs->name, reader->ff().getInt64(fs->name, idx));
                } else if (fs->type == FieldType::Float32) {
                    ff_writer.addFloat32(fs->name, reader->ff().getFloat32(fs->name, idx));
                }
            }
        }
        ff_writer.flush(dir_, new_segment_id);
    }

    // ── Step9: 原子更新 segments_N 注册表 ────────────────────────────────────
    // 找当前最大 generation
    uint32_t max_gen = new_segment_id;
    writeSegmentsFile({new_segment_id}, max_gen + 1);

    // ── Step9: 删除旧 Segment 文件 ────────────────────────────────────────────
    for (uint32_t sid : src_ids) {
        deleteSegmentFiles(sid);
    }

    // 统计输出文件大小
    size_t out_bytes = 0;
    for (const auto& ext : {"tim","doc","pos","fdt","fdx","liv","si"}) {
        std::error_code ec;
        auto sz = std::filesystem::file_size(segPath(new_segment_id, ext), ec);
        if (!ec) out_bytes += sz;
    }

    MergeStats stats;
    stats.input_segment_count = (uint32_t)src_ids.size();
    stats.input_doc_count     = total_input;
    stats.deleted_doc_count   = total_deleted;
    stats.output_doc_count    = output_doc_count;
    stats.output_segment_id   = new_segment_id;
    stats.input_bytes         = 0;  // 原文件已删除，无法统计
    stats.output_bytes        = out_bytes;

    std::cout << "[Merger] Merge complete → segment " << new_segment_id << "\n";
    std::cout << "[Merger] Docs: " << total_input << " → " << output_doc_count
              << " (deleted " << total_deleted << ")\n";
    std::cout << "[Merger] Output size: " << out_bytes / 1024 << " KB\n";
    std::cout << "[Merger] ══════════════════════════════════════════\n\n";

    // 更新本地 reader 列表
    readers_.clear();
    seg_ids_ = {new_segment_id};
    readers_.push_back(std::make_unique<SegmentReader>(dir_, new_segment_id));

    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// deleteSegmentFiles：删除一个 Segment 的全部文件
// ─────────────────────────────────────────────────────────────────────────────

void SegmentMerger::deleteSegmentFiles(uint32_t seg_id) {
    // 删除固定扩展名文件
    for (const auto& ext : {"si","tim","doc","pos","fdt","fdx","liv","pay","nvm","nvd"}) {
        std::error_code ec;
        auto p = segPath(seg_id, ext);
        if (std::filesystem::exists(p, ec)) {
            std::filesystem::remove(p, ec);
            if (!ec) std::cout << "[Merger] Removed " << p << "\n";
        }
    }
    // 删除所有 _N.ff_* 文件（字段名由 Schema 决定，用通配扫描）
    std::string prefix = "_" + std::to_string(seg_id) + ".ff_";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        auto name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0) {
            std::filesystem::remove(entry.path(), ec);
            if (!ec) std::cout << "[Merger] Removed " << entry.path().string() << "\n";
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// writeSegmentsFile：写 segments_N 注册表
// ─────────────────────────────────────────────────────────────────────────────

void SegmentMerger::writeSegmentsFile(const std::vector<uint32_t>& active_ids,
                                       uint32_t generation)
{
    std::string path = dir_ + "/segments_" + std::to_string(generation);
    std::ofstream f(path, std::ios::trunc);
    f << "generation=" << generation << "\n";
    f << "segment_count=" << active_ids.size() << "\n";
    for (uint32_t id : active_ids) f << "segment_" << id << "\n";
    std::cout << "[Merger] Updated " << path << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint32_t> SegmentMerger::activeSegmentIds() const {
    return seg_ids_;
}

void SegmentMerger::printStats() const {
    std::cout << "\n[Merger] Active segments: " << seg_ids_.size() << "\n";
    for (size_t i = 0; i < seg_ids_.size(); ++i) {
        const auto& r = readers_[i];
        uint32_t alive = 0;
        for (uint32_t d = 1; d <= r->docCount(); ++d)
            if (r->isAlive(d)) ++alive;
        std::cout << "  Seg " << seg_ids_[i]
                  << ": total=" << r->docCount()
                  << " alive=" << alive
                  << " deleted=" << (r->docCount() - alive)
                  << " terms=" << r->termCount() << "\n";
    }
}

std::string SegmentMerger::segPath(uint32_t seg_id, const std::string& ext) const {
    return dir_ + "/_" + std::to_string(seg_id) + "." + ext;
}

} // namespace ii
