// tools/doc_reader.cpp
//
// .doc_<field> 索引调试工具（从索引目录加载全部 segment）
//
// 用法（交互模式）:
//   doc_reader --index <index_dir>
//
// 用法（单次操作）:
//   doc_reader --index <dir> --verify <term>
//   doc_reader --index <dir> --skip   <term>
//   doc_reader --index <dir> --count
//
// 交互命令:
//   verify <term>  校验 posting + 打印 (index,doc_id,tf,positions) + 显示原文（term 标红）
//   skip   <term>  输出该 term 的 SkipList 概要
//   :count         全部 term 总数
//   :list  [N]     列出前 N 个 term
//   :quit / :q     退出

#include "analysis/analyzer.h"
#include "codec/pfor_delta.h"
#include "codec/skiplist.h"
#include "core/types.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

static const char* RED   = "\033[1;31m";
static const char* RESET = "\033[0m";

// ─────────────────────────────────────────────────────────────────────────────
// 二进制读取辅助
// ─────────────────────────────────────────────────────────────────────────────

static bool rdU32(std::ifstream& f, uint32_t& v) { return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), 4)); }
static bool rdU64(std::ifstream& f, uint64_t& v) { return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), 8)); }
static bool rdF32(std::ifstream& f, float&    v) { return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), 4)); }
static bool rdStr(std::ifstream& f, std::string& s) {
    uint32_t len = 0;
    if (!rdU32(f, len)) return false;
    s.resize(len);
    return static_cast<bool>(f.read(&s[0], len));
}

// ─────────────────────────────────────────────────────────────────────────────
// 内存数据结构
// ─────────────────────────────────────────────────────────────────────────────

struct TermData {
    ii::TermMeta            meta;
    std::vector<ii::DocId>  doc_ids;
    std::vector<uint8_t>    tfs;
};

struct StoredDoc {
    uint32_t doc_id = 0;
    uint64_t ext_id = 0;
    std::map<std::string, std::string> fields;
};

struct Segment {
    uint32_t    id;
    uint32_t    doc_count;
    std::string dir;

    // field -> (term -> TermData)
    std::map<std::string, std::unordered_map<std::string, TermData>> field_terms;
    std::vector<std::string> ordered_terms;   // "field:term" 去重有序，供 :list 使用

    uint32_t             liv_doc_count = 0;
    std::vector<uint8_t> liv_bitmap;

    std::unordered_map<uint32_t, uint64_t> fdx;  // doc_id -> fdt offset

    bool isAlive(uint32_t doc_id) const {
        if (doc_id == 0 || doc_id > liv_doc_count || liv_bitmap.empty()) return true;
        uint32_t idx = doc_id - 1;
        return (liv_bitmap[idx / 8] >> (idx % 8)) & 1u;
    }

    std::string segPath(const std::string& ext) const {
        return dir + "/_" + std::to_string(id) + "." + ext;
    }
    std::string fieldPath(const std::string& field, const std::string& ext) const {
        return dir + "/_" + std::to_string(id) + "." + ext + "_" + field;
    }
};

struct IndexData {
    std::string          dir;
    std::vector<Segment> segs;
    uint32_t             total_terms = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// posting 解压（与旧版相同）
// ─────────────────────────────────────────────────────────────────────────────

static bool decodePostings(std::ifstream& f, const ii::TermMeta& meta,
                           std::vector<ii::DocId>& out, std::string& err) {
    f.clear();
    f.seekg(static_cast<std::streamoff>(meta.posting_offset));
    if (!f) { err = "seekg failed at posting_offset=" + std::to_string(meta.posting_offset); return false; }

    uint32_t remaining = meta.doc_freq;
    out.reserve(remaining);
    while (remaining > 0) {
        ii::BlockHeader hdr{};
        if (!f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) { err = "read BlockHeader failed"; return false; }
        size_t body = (static_cast<size_t>(hdr.size) * hdr.b + 7) / 8
                    + static_cast<size_t>(hdr.exc_count) * sizeof(ii::PatchEntry);
        std::vector<uint8_t> block(sizeof(ii::BlockHeader) + body);
        std::memcpy(block.data(), &hdr, sizeof(hdr));
        if (!f.read(reinterpret_cast<char*>(block.data() + sizeof(hdr)), body)) { err = "read block body failed"; return false; }
        ii::PForDelta::decompressBlock(block.data(), out);
        remaining -= hdr.size;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 加载单个字段的 tim + doc + tf bytes
// ─────────────────────────────────────────────────────────────────────────────

static bool loadField(Segment& seg, const std::string& field) {
    // --- .tim_<field> ---
    std::ifstream ftim(seg.fieldPath(field, "tim"), std::ios::binary);
    if (!ftim) return false;

    uint32_t term_count = 0;
    rdU32(ftim, term_count);

    auto& terms = seg.field_terms[field];
    terms.reserve(term_count);
    std::vector<std::string> order;
    order.reserve(term_count);

    for (uint32_t i = 0; i < term_count; ++i) {
        std::string term;
        ii::TermMeta m{};
        rdStr(ftim, term);
        rdU32(ftim, m.doc_freq);
        rdU32(ftim, m.total_term_freq);
        rdU64(ftim, m.posting_offset);
        rdU64(ftim, m.skip_offset);
        rdU64(ftim, m.pos_offset);
        rdF32(ftim, m.upper_bound);
        if (ftim.peek() != std::char_traits<char>::eof()) rdU64(ftim, m.tf_data_offset);
        terms[term].meta = m;
        order.push_back(term);
        seg.ordered_terms.push_back(field + ":" + term);
    }

    // --- .doc_<field> ---
    std::ifstream fdoc(seg.fieldPath(field, "doc"), std::ios::binary);
    if (!fdoc) return false;

    for (const auto& term : order) {
        auto& td = terms.at(term);
        std::string err;
        if (!decodePostings(fdoc, td.meta, td.doc_ids, err)) return false;

        if (td.meta.tf_data_offset != 0 && td.meta.doc_freq > 0) {
            fdoc.clear();
            fdoc.seekg(static_cast<std::streamoff>(td.meta.tf_data_offset));
            td.tfs.resize(td.meta.doc_freq);
            fdoc.read(reinterpret_cast<char*>(td.tfs.data()), td.meta.doc_freq);
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 加载 .liv / .fdx
// ─────────────────────────────────────────────────────────────────────────────

static void loadLiv(Segment& seg) {
    std::ifstream f(seg.segPath("liv"), std::ios::binary);
    if (!f) return;
    rdU32(f, seg.liv_doc_count);
    uint32_t bytes = (seg.liv_doc_count + 7) / 8;
    seg.liv_bitmap.resize(bytes);
    f.read(reinterpret_cast<char*>(seg.liv_bitmap.data()), bytes);
}

static void loadFdx(Segment& seg) {
    std::ifstream f(seg.segPath("fdx"), std::ios::binary);
    if (!f) return;
    uint32_t count = 0;
    rdU32(f, count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t doc_id = 0; uint64_t off = 0;
        rdU32(f, doc_id); rdU64(f, off);
        seg.fdx[doc_id] = off;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 从 .fdt 读取存储文档
// ─────────────────────────────────────────────────────────────────────────────

static bool readStoredDoc(const std::string& fdt_path, uint64_t offset, StoredDoc& out) {
    std::ifstream f(fdt_path, std::ios::binary);
    if (!f) return false;
    f.seekg(static_cast<std::streamoff>(offset));
    rdU32(f, out.doc_id);
    rdU64(f, out.ext_id);
    uint32_t fc = 0;
    rdU32(f, fc);
    for (uint32_t i = 0; i < fc; ++i) {
        std::string name, val;
        rdStr(f, name); rdStr(f, val);
        out.fields[name] = val;
    }
    return f.good();
}

// ─────────────────────────────────────────────────────────────────────────────
// 从 .pos_<field> 读取指定 term + doc_id 的 token 位置列表
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<uint32_t> readPositions(const std::string& pos_path,
                                            uint64_t pos_offset, uint32_t doc_freq,
                                            ii::DocId target_doc_id) {
    if (pos_offset == 0 || pos_path.empty()) return {};
    std::ifstream f(pos_path, std::ios::binary);
    if (!f) return {};
    f.seekg(static_cast<std::streamoff>(pos_offset));
    for (uint32_t d = 0; d < doc_freq; ++d) {
        uint32_t did = 0, tf = 0;
        if (!rdU32(f, did) || !rdU32(f, tf)) break;
        if (static_cast<ii::DocId>(did) == target_doc_id) {
            std::vector<uint32_t> pos(tf);
            f.read(reinterpret_cast<char*>(pos.data()), tf * 4);
            return pos;
        }
        f.seekg(static_cast<std::streamoff>(tf * 4), std::ios::cur);
    }
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// 用 Analyzer 重新分析字段文本，在 query_term 出现位置插入 ANSI 红色标记
// ─────────────────────────────────────────────────────────────────────────────

// 根据 .pos 文件中的 token ordinal positions 在原文中标红对应词。
// 策略：charFilter → 精确扫描词边界 → 跳停用词计 ordinal
//       → 用 pos_positions 查 ordinal→char range → 标红原文对应区间。
// 人工评测：标红的词是否确实是 query_term，可判断 pos 记录是否正确。
static std::string highlightField(const std::string& text,
                                   const std::vector<uint32_t>& pos_positions) {
    if (pos_positions.empty()) return text;

    // charFilter：非字母→空格、小写，长度与原文相同，下标一一对应
    std::string cleaned;
    cleaned.reserve(text.size());
    for (unsigned char c : text)
        cleaned += std::isalpha(c) ? static_cast<char>(std::tolower(c)) : ' ';

    // 扫描词边界，对每个词调用 analyzeQuery 判断是否停用词
    // 非停用词分配递增 ordinal；建立 ordinal → (char_start, char_end) 映射
    static ii::Analyzer ana;
    std::unordered_map<uint32_t, std::pair<uint32_t,uint32_t>> ordinal_to_span;

    const char* p = cleaned.data();
    const char* e = p + cleaned.size();
    uint32_t ordinal = 0;
    while (p < e) {
        while (p < e && *p == ' ') ++p;
        const char* ws = p;
        while (p < e && *p != ' ') ++p;
        if (p == ws) break;

        uint32_t cs = static_cast<uint32_t>(ws - cleaned.data());
        uint32_t ce = static_cast<uint32_t>(p  - cleaned.data());
        std::string word(ws, p);

        // analyzeQuery 返回空 → 停用词，不计 ordinal
        if (!ana.analyzeQuery(word).empty()) {
            ordinal_to_span[ordinal] = {cs, ce};
            ++ordinal;
        }
    }

    // 收集 pos_positions 对应的字符区间
    std::vector<std::pair<uint32_t,uint32_t>> ranges;
    for (uint32_t op : pos_positions) {
        auto it = ordinal_to_span.find(op);
        if (it != ordinal_to_span.end())
            ranges.push_back(it->second);
    }
    if (ranges.empty()) return text;
    std::sort(ranges.begin(), ranges.end());

    // 在原文对应区间插入 ANSI 红色标记
    std::string result;
    result.reserve(text.size() + ranges.size() * 16);
    uint32_t cur = 0;
    for (const auto& [s, we] : ranges) {
        uint32_t safe_e = std::min(we, static_cast<uint32_t>(text.size()));
        if (s > cur) result += text.substr(cur, s - cur);
        result += RED;
        result += text.substr(s, safe_e - s);
        result += RESET;
        cur = safe_e;
    }
    if (cur < text.size()) result += text.substr(cur);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 加载整个索引目录
// ─────────────────────────────────────────────────────────────────────────────

static IndexData loadIndex(const std::string& dir) {
    IndexData idx;
    idx.dir = dir;

    // 扫描所有 _N.si 文件，确定 segment id 列表
    std::vector<uint32_t> seg_ids;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.size() >= 4 && name[0] == '_' && name.substr(name.size() - 3) == ".si") {
            try {
                uint32_t sid = std::stoul(name.substr(1, name.size() - 4));
                seg_ids.push_back(sid);
            } catch (...) {}
        }
    }
    std::sort(seg_ids.begin(), seg_ids.end());

    for (uint32_t sid : seg_ids) {
        Segment seg;
        seg.id  = sid;
        seg.dir = dir;

        // 读 .si 获取 doc_count 和 indexed_fields
        std::ifstream fsi(seg.segPath("si"), std::ios::binary);
        if (!fsi) continue;
        uint32_t dummy = 0;
        rdU32(fsi, dummy);                   // segment_id
        rdU32(fsi, seg.doc_count);
        rdU32(fsi, dummy);                   // term_count
        std::string created_at, indexed_fields;
        if (fsi.good()) rdStr(fsi, created_at);
        if (fsi.good() && fsi.peek() != std::char_traits<char>::eof())
            rdStr(fsi, indexed_fields);

        // 拆分字段列表
        std::vector<std::string> fields;
        for (size_t p = 0; p <= indexed_fields.size(); ) {
            size_t q = indexed_fields.find(',', p);
            if (q == std::string::npos) q = indexed_fields.size();
            if (q > p) fields.push_back(indexed_fields.substr(p, q - p));
            p = q + 1;
        }

        for (const auto& field : fields)
            loadField(seg, field);

        loadLiv(seg);
        loadFdx(seg);

        // 去重 ordered_terms（多字段可能有同名 term）
        std::set<std::string> seen;
        std::vector<std::string> deduped;
        for (const auto& t : seg.ordered_terms)
            if (seen.insert(t).second) deduped.push_back(t);
        seg.ordered_terms = std::move(deduped);

        idx.total_terms += static_cast<uint32_t>(seg.ordered_terms.size());
        idx.segs.push_back(std::move(seg));
    }
    return idx;
}

// ─────────────────────────────────────────────────────────────────────────────
// Feature 2/3 合并：verify <term>
// ─────────────────────────────────────────────────────────────────────────────

static void cmdVerify(const IndexData& idx, const std::string& term) {
    bool found = false;

    for (const auto& seg : idx.segs) {
        for (const auto& [field, terms] : seg.field_terms) {
            auto it = terms.find(term);
            if (it == terms.end()) continue;
            found = true;

            const auto& td   = it->second;
            const auto& meta = td.meta;
            const auto& mem  = td.doc_ids;

            std::cout << "=== segment " << seg.id << "  field=" << field
                      << "  df=" << meta.doc_freq << " ===\n";

            // 从文件重新解压 doc_ids + tf bytes（校验用）
            std::ifstream fdoc(seg.fieldPath(field, "doc"), std::ios::binary);
            std::vector<ii::DocId> fresh_ids;
            std::vector<uint8_t>   fresh_tfs;
            std::string err;
            if (fdoc && decodePostings(fdoc, meta, fresh_ids, err)) {
                if (meta.tf_data_offset != 0 && meta.doc_freq > 0) {
                    fdoc.clear();
                    fdoc.seekg(static_cast<std::streamoff>(meta.tf_data_offset));
                    fresh_tfs.resize(meta.doc_freq);
                    fdoc.read(reinterpret_cast<char*>(fresh_tfs.data()), meta.doc_freq);
                }
            }

            // 内存 vs 文件一致性
            bool ids_ok = (fresh_ids.size() == mem.size());
            size_t first_diff = SIZE_MAX;
            if (ids_ok) {
                for (size_t i = 0; i < mem.size(); ++i) {
                    if (fresh_ids[i] != mem[i]) { first_diff = i; ids_ok = false; break; }
                }
            }
            if (!ids_ok) {
                if (first_diff != SIZE_MAX)
                    std::cout << "[MISMATCH] first diff index=" << first_diff
                              << "  mem=" << mem[first_diff] << "  file=" << fresh_ids[first_diff] << "\n";
                else
                    std::cout << "[MISMATCH] count mem=" << mem.size() << " file=" << fresh_ids.size() << "\n";
            } else {
                std::cout << "[OK] memory == file  doc_count=" << mem.size() << "\n";
            }

            // 表头
            bool has_tf  = !fresh_tfs.empty();
            std::string pos_path = seg.fieldPath(field, "pos");
            bool has_pos = fs::exists(pos_path) && meta.pos_offset != 0;
            std::string fdt_path = seg.segPath("fdt");

            std::cout << "  " << std::setw(5)  << "idx"
                      << "  " << std::setw(10) << "doc_id"
                      << "  " << std::setw(4)  << "tf";
            if (has_pos) std::cout << "  positions";
            std::cout << "\n"
                      << "  " << std::string(5,  '-')
                      << "  " << std::string(10, '-')
                      << "  " << std::string(4,  '-');
            if (has_pos) std::cout << "  " << std::string(20, '-');
            std::cout << "\n";

            for (size_t i = 0; i < fresh_ids.size(); ++i) {
                ii::DocId did = fresh_ids[i];
                uint32_t  tf  = has_tf ? static_cast<uint32_t>(fresh_tfs[i]) : 0;

                std::cout << "  " << std::setw(5)  << i
                          << "  " << std::setw(10) << did
                          << "  " << std::setw(4)  << tf;

                // 位置列表
                std::vector<uint32_t> positions;
                if (has_pos) {
                    positions = readPositions(pos_path, meta.pos_offset, meta.doc_freq, did);
                    std::cout << "  [";
                    for (size_t pi = 0; pi < positions.size(); ++pi) {
                        if (pi) std::cout << ',';
                        std::cout << positions[pi];
                    }
                    std::cout << "]";
                }
                std::cout << "\n";

                // 存活检查 + 原文展示
                bool alive = seg.isAlive(static_cast<uint32_t>(did));
                if (!alive) {
                    std::cout << "    [DELETED]\n";
                    continue;
                }

                auto fdx_it = seg.fdx.find(static_cast<uint32_t>(did));
                if (fdx_it == seg.fdx.end()) continue;

                StoredDoc sdoc;
                if (!readStoredDoc(fdt_path, fdx_it->second, sdoc)) continue;

                for (const auto& [fname, fval] : sdoc.fields) {
                    // 多值字段按 \x1f 分割
                    std::string display;
                    bool first_val = true;
                    size_t vp = 0;
                    while (true) {
                        size_t vq = fval.find('\x1f', vp);
                        std::string part = fval.substr(vp, vq == std::string::npos ? std::string::npos : vq - vp);
                        // 对与 query field 相同的字段做高亮
                        std::string rendered = (fname == field) ? highlightField(part, positions) : part;
                        if (!first_val) display += " | ";
                        display += rendered;
                        first_val = false;
                        if (vq == std::string::npos) break;
                        vp = vq + 1;
                    }
                    std::cout << "    @" << fname << ": " << display << "\n";
                }
            }
            std::cout << "\n";
        }
    }

    if (!found) std::cout << "[not found] \"" << term << "\"\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// skip <term>：SkipList 概要
// ─────────────────────────────────────────────────────────────────────────────

static void cmdSkip(const IndexData& idx, const std::string& term) {
    bool found = false;
    for (const auto& seg : idx.segs) {
        for (const auto& [field, terms] : seg.field_terms) {
            auto it = terms.find(term);
            if (it == terms.end()) continue;
            found = true;

            const auto& td   = it->second;
            const auto& meta = td.meta;

            std::ifstream f(seg.fieldPath(field, "doc"), std::ios::binary);
            if (!f) { std::cout << "[error] cannot open doc file\n"; continue; }

            f.seekg(static_cast<std::streamoff>(meta.skip_offset));
            uint32_t l0c = 0, l1c = 0;
            f.read(reinterpret_cast<char*>(&l0c), 4);
            f.read(reinterpret_cast<char*>(&l1c), 4);
            size_t total = 8 + static_cast<size_t>(l0c + l1c) * sizeof(ii::SkipNode);
            std::vector<uint8_t> buf(total);
            f.seekg(static_cast<std::streamoff>(meta.skip_offset));
            f.read(reinterpret_cast<char*>(buf.data()), total);
            ii::SkipList sl = ii::SkipList::deserialize(buf.data(), buf.size());
            if (sl.empty()) { std::cout << "  [no blocks]\n"; continue; }

            float min_s = sl.node(0).max_score, max_s = min_s;
            double sum_s = 0; uint32_t total_docs = 0;
            for (size_t i = 0; i < sl.size(); ++i) {
                const auto& n = sl.node(i);
                min_s  = std::min(min_s, n.max_score);
                max_s  = std::max(max_s, n.max_score);
                sum_s += n.max_score;
                total_docs += n.doc_count;
            }

            std::cout << std::fixed << std::setprecision(6)
                      << "  segment         : " << seg.id          << "\n"
                      << "  field           : " << field           << "\n"
                      << "  term            : " << term            << "\n"
                      << "  block_count     : " << sl.size()       << "\n"
                      << "  total_docs      : " << total_docs      << "\n";
            if (!td.doc_ids.empty())
                std::cout << "  doc_id range    : " << td.doc_ids.front()
                          << " ~ " << td.doc_ids.back() << "\n";
            std::cout << "  max_score min   : " << min_s                   << "\n"
                      << "  max_score max   : " << max_s                   << "\n"
                      << "  max_score avg   : " << sum_s / sl.size()       << "\n"
                      << "  skip_offset     : " << meta.skip_offset        << "\n"
                      << "  posting_offset  : " << meta.posting_offset     << "\n\n";
        }
    }
    if (!found) std::cout << "[not found] \"" << term << "\"\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// 交互循环
// ─────────────────────────────────────────────────────────────────────────────

static std::pair<std::string, std::string> splitCmd(const std::string& line) {
    size_t sp = line.find(' ');
    if (sp == std::string::npos) return {line, {}};
    std::string arg = line.substr(sp + 1);
    size_t s = arg.find_first_not_of(' ');
    return {line.substr(0, sp), s == std::string::npos ? std::string{} : arg.substr(s)};
}

static void interactiveLoop(const IndexData& idx) {
    uint32_t seg_cnt = static_cast<uint32_t>(idx.segs.size());
    std::cout << "Loaded " << seg_cnt << " segment(s), "
              << idx.total_terms << " terms.\n"
              << "Commands: verify <term>  skip <term>  :count  :list [N]  :quit\n";

    // 收集全部去重 term（跨段）
    std::vector<std::string> all_terms;
    {
        std::set<std::string> seen;
        for (const auto& seg : idx.segs)
            for (const auto& t : seg.ordered_terms)
                if (seen.insert(t).second) all_terms.push_back(t);
    }

    std::string line;
    while (true) {
        std::cout << "doc> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        line = line.substr(s, e - s + 1);
        if (line.empty()) continue;

        if (line == ":quit" || line == ":q") break;

        auto [cmd, arg] = splitCmd(line);

        if (cmd == ":count") {
            std::cout << "term_count: " << idx.total_terms
                      << "  (across " << seg_cnt << " segment(s))\n";
        } else if (cmd == ":list") {
            uint32_t n = 20;
            if (!arg.empty()) { try { n = std::stoul(arg); } catch (...) {} }
            uint32_t limit = std::min(n, static_cast<uint32_t>(all_terms.size()));
            for (uint32_t i = 0; i < limit; ++i)
                std::cout << "  [" << std::setw(6) << i << "]  " << all_terms[i] << "\n";
            if (limit < static_cast<uint32_t>(all_terms.size()))
                std::cout << "  ... (" << all_terms.size() - limit << " more)\n";
        } else if (cmd == "verify") {
            if (arg.empty()) std::cout << "usage: verify <term>\n";
            else             cmdVerify(idx, arg);
        } else if (cmd == "skip") {
            if (arg.empty()) std::cout << "usage: skip <term>\n";
            else             cmdSkip(idx, arg);
        } else {
            std::cout << "unknown: " << cmd
                      << "  (verify / skip / :count / :list / :quit)\n";
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

static void usage() {
    std::cerr <<
        "Usage:\n"
        "  doc_reader --index <dir>                  # interactive\n"
        "  doc_reader --index <dir> --verify <term>  # verify posting + show doc\n"
        "  doc_reader --index <dir> --skip   <term>  # skiplist summary\n"
        "  doc_reader --index <dir> --count          # term count\n";
}

int main(int argc, char* argv[]) {
    std::string index_dir;
    std::string verify_term, skip_term;
    bool do_count = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--index"  && i+1 < argc) index_dir   = argv[++i];
        else if (a == "--verify" && i+1 < argc) verify_term = argv[++i];
        else if (a == "--skip"   && i+1 < argc) skip_term   = argv[++i];
        else if (a == "--count")                do_count    = true;
        else { std::cerr << "Unknown arg: " << a << "\n"; usage(); return 1; }
    }

    if (index_dir.empty()) {
        std::cerr << "Error: --index is required\n"; usage(); return 1;
    }

    if (!fs::exists(index_dir)) {
        std::cerr << "Error: directory not found: " << index_dir << "\n"; return 1;
    }

    std::cout << "Loading index from: " << index_dir << " ...\n";
    IndexData idx = loadIndex(index_dir);

    if (idx.segs.empty()) {
        std::cerr << "Error: no segments found in " << index_dir << "\n"; return 1;
    }
    std::cout << "Loaded " << idx.segs.size() << " segment(s), "
              << idx.total_terms << " terms.\n\n";

    if (do_count) {
        std::cout << "term_count: " << idx.total_terms << "\n";
    } else if (!verify_term.empty()) {
        cmdVerify(idx, verify_term);
    } else if (!skip_term.empty()) {
        cmdSkip(idx, skip_term);
    } else {
        interactiveLoop(idx);
    }
    return 0;
}
