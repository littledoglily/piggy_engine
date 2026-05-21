#include "index/segment_merger.h"
#include "field/fast_field_writer.h"
#include "field/schema.h"
#include <cmath>
#include "codec/pfor_delta.h"
#include "codec/skiplist.h"
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

    // ── Step2: 收集所有字段（所有 Segment 的 indexedFieldNames 取并集）──────
    Schema schema = Schema::load(dir_);

    std::vector<std::string> all_fields;
    {
        std::set<std::string> fset;
        for (uint32_t sid : src_ids) {
            for (auto& r : readers_) {
                if (r->segmentId() != sid) continue;
                for (const auto& f : r->indexedFieldNames())
                    fset.insert(f);
            }
        }
        all_fields.assign(fset.begin(), fset.end());
    }
    std::cout << "[Merger] Indexed fields: " << all_fields.size() << "\n";

    // ── Step3: 对每个字段独立执行 term 多路归并 ────────────────────────────────
    //
    // per-field 版本：每个字段产生独立的 _N.tim_<field> / .doc_<field> / .pos_<field>
    // FreqsOnly 字段：从 PostingIterator 读 doc_ids，位置数据不存在，tf 设为 1
    // FreqsPositions 字段：从 readPosEntries(field, term) 读 doc_ids+tf+positions

    struct NewTermMeta {
        uint32_t df, total_tf;
        uint64_t posting_offset, skip_offset, pos_offset;
        float    upper_bound;
        uint64_t tf_data_offset = 0;
    };

    const float k1 = 1.2f, b = 0.75f;
    uint32_t total_term_count = 0;

    for (const auto& field : all_fields) {
        const FieldSchema* fs = schema.find(field);
        bool has_positions = fs && (fs->index == IndexOption::FreqsPositions);

        // 2a. 收集该字段的全部 term（各 Segment 并集）
        std::set<std::string> field_terms;
        for (uint32_t sid : src_ids) {
            for (auto& r : readers_) {
                if (r->segmentId() != sid) continue;
                const auto* fdict = r->fieldTermDict(field);
                if (!fdict) continue;
                for (const auto& [term, _] : *fdict)
                    field_terms.insert(term);
            }
        }
        if (field_terms.empty()) continue;

        // 2b-pre. 计算合并后 per-doc 字段长度 和 avgdl（用于 BM25 UB 重算）
        std::map<DocId, uint32_t> merged_doc_lens;
        for (const auto& gd : alive_docs) {
            SegmentReader* reader = nullptr;
            for (auto& r : readers_)
                if (r->segmentId() == gd.seg_id) { reader = r.get(); break; }
            if (!reader) continue;
            uint64_t key = ((uint64_t)gd.seg_id << 20) | gd.orig_doc_id;
            auto it = remap.find(key);
            if (it == remap.end()) continue;
            uint32_t dl = reader->fieldDocLen(field, gd.orig_doc_id);
            merged_doc_lens[it->second] = dl;
        }
        float merged_avgdl = 0.f;
        if (!merged_doc_lens.empty()) {
            uint64_t total_len = 0;
            for (auto& [did, l] : merged_doc_lens) total_len += l;
            merged_avgdl = (float)total_len / (float)merged_doc_lens.size();
        }

        // 2b. 打开输出文件
        std::ofstream fdoc(segFieldPath(new_segment_id, field, "doc"),
                           std::ios::binary|std::ios::trunc);
        std::ofstream fpos;
        if (has_positions)
            fpos.open(segFieldPath(new_segment_id, field, "pos"),
                      std::ios::binary|std::ios::trunc);
        if (!fdoc) throw std::runtime_error("Cannot create .doc_" + field);

        std::map<std::string, NewTermMeta> field_term_dict;

        // 2c. 对每个 term 多路归并
        for (const auto& term : field_terms) {
            std::vector<std::pair<DocId, uint32_t>>          merged;     // <new_doc_id, tf>
            std::vector<std::pair<DocId, std::vector<Pos>>>  merged_pos; // <new_doc_id, positions>

            for (uint32_t sid : src_ids) {
                SegmentReader* reader = nullptr;
                for (auto& r : readers_) {
                    if (r->segmentId() == sid) { reader = r.get(); break; }
                }
                if (!reader) continue;

                if (has_positions) {
                    // FreqsPositions：readPosEntries 提供 doc_id + tf + positions
                    for (const auto& entry : reader->readPosEntries(field, term)) {
                        uint64_t key = ((uint64_t)sid << 20) | entry.doc_id;
                        auto it = remap.find(key);
                        if (it == remap.end()) continue;  // 已删除
                        merged.push_back({it->second, entry.tf});
                        merged_pos.push_back({it->second, entry.positions});
                    }
                } else {
                    // FreqsOnly：PostingIterator 提供 doc_ids + tf
                    auto iter = reader->postingIterator(field, term);
                    while (!iter.isEnd()) {
                        uint64_t key = ((uint64_t)sid << 20) | iter.docId();
                        auto it = remap.find(key);
                        if (it != remap.end())
                            merged.push_back({it->second, iter.tf()});
                        iter.next();
                    }
                }
            }

            if (merged.empty()) continue;

            std::sort(merged.begin(), merged.end());
            if (has_positions) std::sort(merged_pos.begin(), merged_pos.end());

            // 重算 IDF，使用 b=0.75 完整 BM25 上界
            uint32_t df  = (uint32_t)merged.size();
            float idf    = std::log(1.0f + (float)(output_doc_count - df + 0.5f)
                                          / (float)(df + 0.5f));
            float max_tn = 0.f;
            for (auto& [did, tf] : merged) {
                float tf_f = (float)tf;
                float dl   = (float)merged_doc_lens.count(did) ? (float)merged_doc_lens[did] : merged_avgdl;
                float denom = tf_f + k1 * (1.f - b + b * (merged_avgdl > 0.f ? dl / merged_avgdl : 1.f));
                max_tn = std::max(max_tn, tf_f * (k1 + 1.f) / denom);
            }
            float ub = max_tn * idf;

            std::vector<DocId> new_doc_ids;
            new_doc_ids.reserve(merged.size());
            for (auto& [did, _tf] : merged) new_doc_ids.push_back(did);

            constexpr int BLOCK_SZ = 128;
            std::vector<float> block_ubs;
            block_ubs.reserve((merged.size() + BLOCK_SZ - 1) / BLOCK_SZ);
            for (size_t bi = 0; bi < merged.size(); bi += BLOCK_SZ) {
                size_t bend = std::min(bi + (size_t)BLOCK_SZ, merged.size());
                float blk_max = 0.f;
                for (size_t j = bi; j < bend; ++j) {
                    float tf_f = (float)merged[j].second;
                    float dl   = (float)merged_doc_lens.count(merged[j].first)
                                 ? (float)merged_doc_lens[merged[j].first] : merged_avgdl;
                    float denom = tf_f + k1 * (1.f - b + b * (merged_avgdl > 0.f ? dl / merged_avgdl : 1.f));
                    blk_max = std::max(blk_max, tf_f * (k1 + 1.f) / denom);
                }
                block_ubs.push_back(blk_max);
            }

            std::vector<SkipNode> skip_nodes;
            auto compressed = PForDelta::compress(new_doc_ids, skip_nodes, block_ubs);
            SkipList sl(skip_nodes);
            auto sl_bytes = sl.serialize();

            NewTermMeta ntm;
            ntm.df             = df;
            ntm.total_tf       = 0;
            for (auto& [did, tf] : merged) ntm.total_tf += tf;
            ntm.skip_offset    = (uint64_t)fdoc.tellp();
            ntm.posting_offset = ntm.skip_offset + sl_bytes.size();
            ntm.pos_offset     = has_positions ? (uint64_t)fpos.tellp() : 0;
            ntm.upper_bound    = ub;

            wBytes(fdoc, sl_bytes);
            wBytes(fdoc, compressed);

            // tf 字节数组（按 merged 升序排列，与 doc_ids 对应）
            ntm.tf_data_offset = (uint64_t)fdoc.tellp();
            for (auto& [did, tf] : merged) {
                uint8_t tb = (uint8_t)std::min(tf, 255u);
                fdoc.write((char*)&tb, 1);
            }

            if (has_positions) {
                for (auto& [did, positions] : merged_pos) {
                    wU32(fpos, did);
                    wU32(fpos, (uint32_t)positions.size());
                    for (Pos p : positions) wU32(fpos, p);
                }
            }

            field_term_dict[term] = ntm;
        }

        // 2d. 写 _N.tim_<field>
        {
            std::ofstream ftim(segFieldPath(new_segment_id, field, "tim"),
                               std::ios::binary|std::ios::trunc);
            wU32(ftim, (uint32_t)field_term_dict.size());
            for (const auto& [term, m] : field_term_dict) {
                wStr(ftim, term);
                wU32(ftim, m.df);
                wU32(ftim, m.total_tf);
                wU64(ftim, m.posting_offset);
                wU64(ftim, m.skip_offset);
                wU64(ftim, m.pos_offset);
                wF32(ftim, m.upper_bound);
                wU64(ftim, m.tf_data_offset);
            }
        }

        // 2e. 写 _N.len_<field>（per-doc 字段长度，uint16_t 数组）
        {
            std::ofstream flen(segFieldPath(new_segment_id, field, "len"),
                               std::ios::binary|std::ios::trunc);
            wU32(flen, output_doc_count);
            for (DocId new_doc = 1; new_doc <= output_doc_count; ++new_doc) {
                uint16_t l = merged_doc_lens.count(new_doc)
                             ? (uint16_t)std::min(merged_doc_lens[new_doc], 65535u) : 0u;
                flen.write((char*)&l, 2);
            }
        }

        total_term_count += (uint32_t)field_term_dict.size();
        std::cout << "[Merger] Field \"" << field << "\": "
                  << field_terms.size() << " terms merged\n";
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

    // ── Step7: 写新 .si（含 indexed_fields，供 SegmentReader 发现 per-field 文件）
    {
        std::ofstream fsi(segPath(new_segment_id, "si"),
                          std::ios::binary|std::ios::trunc);
        wU32(fsi, new_segment_id);
        wU32(fsi, output_doc_count);
        wU32(fsi, total_term_count);
        wStr(fsi, std::string("merged"));

        std::string ifields;
        for (size_t i = 0; i < all_fields.size(); ++i) {
            if (i) ifields += ',';
            ifields += all_fields[i];
        }
        wStr(fsi, ifields);
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

    // 统计输出文件大小（per-field 文件 + 共享文件）
    size_t out_bytes = 0;
    for (const auto& ext : {"fdt","fdx","liv","si"}) {
        std::error_code ec;
        auto sz = std::filesystem::file_size(segPath(new_segment_id, ext), ec);
        if (!ec) out_bytes += sz;
    }
    // per-field 文件（tim_<field>, doc_<field>, pos_<field>）
    {
        std::error_code ec;
        std::string prefix = "_" + std::to_string(new_segment_id) + ".";
        for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
            auto name = entry.path().filename().string();
            if (name.rfind(prefix, 0) == 0) {
                auto sz = std::filesystem::file_size(entry.path(), ec);
                if (!ec) out_bytes += sz;
            }
        }
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
    // 删除共享文件（legacy 或 per-field 共享）
    for (const auto& ext : {"si","tim","doc","pos","fdt","fdx","liv","pay","nvm","nvd"}) {
        std::error_code ec;
        auto p = segPath(seg_id, ext);
        if (std::filesystem::exists(p, ec)) {
            std::filesystem::remove(p, ec);
            if (!ec) std::cout << "[Merger] Removed " << p << "\n";
        }
    }
    // 删除所有 _N.<ext>_<field> 文件（tim_*, doc_*, pos_*, ff_* 等）
    std::string seg_prefix = "_" + std::to_string(seg_id) + ".";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir_, ec)) {
        auto name = entry.path().filename().string();
        if (name.rfind(seg_prefix, 0) == 0) {
            // 只删除含下划线后缀的文件（即 per-field 格式：.tim_title, .ff_pubtime …）
            auto rest = name.substr(seg_prefix.size());
            if (rest.find('_') != std::string::npos) {
                std::filesystem::remove(entry.path(), ec);
                if (!ec) std::cout << "[Merger] Removed " << entry.path().string() << "\n";
            }
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

std::string SegmentMerger::segFieldPath(uint32_t seg_id, const std::string& field,
                                         const std::string& ext) const {
    return dir_ + "/_" + std::to_string(seg_id) + "." + ext + "_" + field;
}

} // namespace ii
