#include "index/segment_merger.h"
#include "field/fast_field_writer.h"
#include "field/schema.h"
#include "common/kway_merge.h"
#include "common/file_utils.h"
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
    seg_ids_ = file_utils::listSegmentIds(dir_);
    for (uint32_t id : seg_ids_)
        readers_.push_back(std::make_unique<SegmentReader>(dir_, id));
}

SegmentMerger::SegmentMerger(const std::string& dir,
                               const std::vector<uint32_t>& seg_ids)
    : dir_(dir), seg_ids_(seg_ids)
{
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
    // 创建输出 segment 目录并标记合并进行中
    const std::string out_seg_dir = dir_ + "/segment_" + std::to_string(new_segment_id);
    std::filesystem::create_directories(out_seg_dir);
    { std::ofstream ing(out_seg_dir + "/.ing"); }

    std::cout << "\n[Merger] ══════════════════════════════════════════\n";
    std::cout << "[Merger] Starting merge: segments [";
    for (size_t i = 0; i < src_ids.size(); ++i) {
        std::cout << src_ids[i]; if (i+1<src_ids.size()) std::cout << ",";
    }
    std::cout << "] → segment " << new_segment_id << "\n";

    // ── Step1: 统计存活文档，建立全局 doc_id 重映射表 ─────────────────────────
    // GlobalDoc 只存元数据，不存储字段原文（避免 O(N×doc_size) 内存峰值）：
    //   orig_doc_id : .fdt 中记录的全局 doc_id（供 remap / fieldDocLen 查询）
    //   local_pos   : segment 内 1-indexed 位置（供 readStoredDoc 惰性读 / FastField 索引）
    //   new_doc_id  : 合并后分配的新 doc_id（避免 Step5/Step8 重复 remap 查询）
    struct GlobalDoc {
        uint32_t seg_id;
        DocId    orig_doc_id;  // .fdt 存储的全局 doc_id
        uint32_t local_pos;    // 1-indexed segment 内位置
        DocId    new_doc_id;   // 合并后新 doc_id
    };
    std::vector<GlobalDoc> alive_docs;

    uint32_t total_input  = 0;
    uint32_t total_deleted= 0;

    // key = (seg_id << 20) | orig_doc_id，与 posting list 中的 doc_id 对齐
    std::unordered_map<uint64_t, DocId> remap;

    DocId new_id = 1;
    for (uint32_t sid : src_ids) {
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
            // 只读 orig_doc_id（4B），不读 str_fields
            auto stored = reader->readStoredDoc(local);
            uint64_t key = ((uint64_t)sid << 20) | stored.doc_id;
            remap[key] = new_id;
            alive_docs.push_back({sid, stored.doc_id, local, new_id});
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
        // 用 vector 代替 map：4B/doc vs 48B/doc，1500万文档省 ~660MB
        std::vector<uint32_t> merged_doc_lens(output_doc_count + 1, 0); // 下标 = new_doc_id
        uint64_t total_field_len = 0;
        for (const auto& gd : alive_docs) {
            SegmentReader* reader = nullptr;
            for (auto& r : readers_)
                if (r->segmentId() == gd.seg_id) { reader = r.get(); break; }
            if (!reader) continue;
            uint32_t dl = reader->fieldDocLen(field, gd.orig_doc_id);
            merged_doc_lens[gd.new_doc_id] = dl;
            total_field_len += dl;
        }
        float merged_avgdl = output_doc_count > 0
            ? (float)total_field_len / (float)output_doc_count : 0.f;

        // 2b. 打开输出文件
        std::ofstream fdoc(segFieldPath(new_segment_id, field, "doc"),
                           std::ios::binary|std::ios::trunc);
        std::ofstream fpos;
        if (has_positions)
            fpos.open(segFieldPath(new_segment_id, field, "pos"),
                      std::ios::binary|std::ios::trunc);
        if (!fdoc) throw std::runtime_error("Cannot create .doc_" + field);

        std::map<std::string, NewTermMeta> field_term_dict;

        // 2c. 对每个 term 多路归并（K-way merge，输出已有序，无需 sort）
        //
        // MergeDoc：KwayMerge 的元素类型，按 new_doc_id 升序排列
        struct MergeDoc {
            DocId            new_doc_id;
            uint32_t         tf;
            std::vector<Pos> positions;   // FreqsOnly 时为空
            bool operator<(const MergeDoc& o) const { return new_doc_id < o.new_doc_id; }
        };

        for (const auto& term : field_terms) {
            KwayMerge<MergeDoc> kmerge;

            for (uint32_t sid : src_ids) {
                SegmentReader* reader = nullptr;
                for (auto& r : readers_) {
                    if (r->segmentId() == sid) { reader = r.get(); break; }
                }
                if (!reader) continue;

                if (has_positions) {
                    // FreqsPositions：PosIterator 流式读，常数内存
                    // 用 shared_ptr 包装 move-only 迭代器，使 lambda 满足 std::function 的可拷贝要求
                    auto iter_ptr = std::make_shared<PosIterator>(reader->posIterator(field, term));
                    if (iter_ptr->isEnd()) continue;
                    kmerge.addSource(
                        [iter_ptr, sid, &remap]() mutable
                        -> std::optional<MergeDoc> {
                            auto& iter = *iter_ptr;
                            while (!iter.isEnd()) {
                                uint64_t key = ((uint64_t)sid << 20) | iter.docId();
                                auto it = remap.find(key);
                                uint32_t tf  = iter.tf();
                                auto     pos = iter.takePositions();
                                iter.next();
                                if (it != remap.end())
                                    return MergeDoc{it->second, tf, std::move(pos)};
                            }
                            return std::nullopt;
                        });
                } else {
                    // FreqsOnly：PostingIterator 已是惰性块读取
                    auto iter_ptr = std::shared_ptr<IPostingIterator>(reader->postingIterator(field, term));
                    if (iter_ptr->isEnd()) continue;
                    kmerge.addSource(
                        [iter_ptr, sid, &remap]() mutable
                        -> std::optional<MergeDoc> {
                            auto& iter = *iter_ptr;
                            while (!iter.isEnd()) {
                                uint64_t key = ((uint64_t)sid << 20) | iter.docId();
                                auto it = remap.find(key);
                                uint32_t tf = iter.tf();
                                iter.next();
                                if (it != remap.end())
                                    return MergeDoc{it->second, tf, {}};
                            }
                            return std::nullopt;
                        });
                }
            }

            kmerge.init();

            // 从 K-way heap 拉取，输出已按 new_doc_id 升序，无需额外 sort
            std::vector<std::pair<DocId, uint32_t>>          merged;
            std::vector<std::pair<DocId, std::vector<Pos>>>  merged_pos;
            MergeDoc doc;
            while (kmerge.next(doc)) {
                merged.push_back({doc.new_doc_id, doc.tf});
                if (has_positions)
                    merged_pos.push_back({doc.new_doc_id, std::move(doc.positions)});
            }

            if (merged.empty()) continue;

            // 重算 IDF，使用 b=0.75 完整 BM25 上界
            uint32_t df  = (uint32_t)merged.size();
            float idf    = std::log(1.0f + (float)(output_doc_count - df + 0.5f)
                                          / (float)(df + 0.5f));
            float max_tn = 0.f;
            for (auto& [did, tf] : merged) {
                float tf_f = (float)tf;
                uint32_t raw = (did < merged_doc_lens.size()) ? merged_doc_lens[did] : 0;
                float dl   = (raw > 0) ? (float)raw : merged_avgdl;
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
                    DocId jdid = merged[j].first;
                    uint32_t raw = (jdid < merged_doc_lens.size()) ? merged_doc_lens[jdid] : 0;
                    float dl   = (raw > 0) ? (float)raw : merged_avgdl;
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
                uint16_t l = (new_doc < merged_doc_lens.size())
                             ? (uint16_t)std::min(merged_doc_lens[new_doc], 65535u) : 0u;
                flen.write((char*)&l, 2);
            }
        }

        total_term_count += (uint32_t)field_term_dict.size();
        std::cout << "[Merger] Field \"" << field << "\": "
                  << field_terms.size() << " terms merged\n";
    }

    // ── Step5: 写新 .fdt / .fdx（惰性读：每条文档临时读取，常数内存）────────────
    std::vector<uint64_t> fdx_offsets;
    {
        std::ofstream ffdt(segPath(new_segment_id, "fdt"),
                           std::ios::binary|std::ios::trunc);
        for (const auto& gd : alive_docs) {
            SegmentReader* reader = nullptr;
            for (auto& r : readers_)
                if (r->segmentId() == gd.seg_id) { reader = r.get(); break; }

            // 从原始 .fdt 惰性读一条，处理完即释放
            auto stored = reader->readStoredDoc(gd.local_pos);
            fdx_offsets.push_back((uint64_t)ffdt.tellp());
            wU32(ffdt, gd.new_doc_id);
            wU64(ffdt, stored.ext_id);
            wU32(ffdt, (uint32_t)stored.str_fields.size());
            for (const auto& [name, val] : stored.str_fields) {
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
            uint32_t idx = gd.local_pos - 1;  // 0-indexed segment 内位置
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

    // ── Step9: 删除旧 Segment 文件 ────────────────────────────────────────────
    for (uint32_t sid : src_ids) {
        deleteSegmentFiles(sid);
    }

    // 合并完成：写 .done，移除 .ing
    { std::ofstream done(out_seg_dir + "/.done"); }
    std::filesystem::remove(out_seg_dir + "/.ing");

    // 统计输出文件大小（遍历 segment_N/ 子目录，跳过 .ing/.done 状态文件）
    size_t out_bytes = 0;
    {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(out_seg_dir, ec)) {
            const auto name = entry.path().filename().string();
            if (name == ".ing" || name == ".done") continue;
            auto sz = std::filesystem::file_size(entry.path(), ec);
            if (!ec) out_bytes += sz;
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
    std::filesystem::path seg_dir = dir_ + "/segment_" + std::to_string(seg_id);
    std::error_code ec;
    // 先列出子文件打印日志，再整体删除
    if (std::filesystem::exists(seg_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(seg_dir, ec))
            std::cout << "[Merger] Removed " << entry.path().string() << "\n";
        std::filesystem::remove_all(seg_dir, ec);
    }
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
    return dir_ + "/segment_" + std::to_string(seg_id) + "/" + ext;
}

std::string SegmentMerger::segFieldPath(uint32_t seg_id, const std::string& field,
                                         const std::string& ext) const {
    return dir_ + "/segment_" + std::to_string(seg_id) + "/" + ext + "_" + field;
}

} // namespace ii
