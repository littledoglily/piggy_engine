// tools/wiki_indexer.cpp
//
// 用法:
//   wiki_indexer --input <wiki_dir> [options]
//
// Options:
//   --input   <path>  Wiki JSONL 数据目录（递归遍历，必填）
//   --output  <path>  索引输出目录（默认 ./wiki_index）
//   --schema  <path>  Schema JSON 文件路径（不传则优先加载 output 目录中的
//                     schema.json，均不存在时回退 defaultSchema）
//   --workers <N>     并行构建线程数（默认 1 = 单线程 IndexWriter）
//   --ram     <MB>    每个 Worker 的 RAM flush 阈值（默认 128）
//   --limit   <N>     最多索引 N 篇文档（默认不限）
//   --top     <N>     打印 posting 详情的 term 数（默认 50，按 df 降序）
//   --verbose         同时打印 top-N 之外所有 term 的 df/ttf/UB（可能数百万行，建议重定向到文件）
//
// 输入格式: JSON Lines，每行一个 JSON 对象
//   字段名以 schema 定义为准；wiki 格式特殊映射：body←text，source←url
// 输出: 构建完成后打印每个 term 的 df / ttf / UB，并对 top-N 展示 posting list 样本

#include "index/index_writer.h"
#include "index/parallel_index_writer.h"
#include "index/segment_reader.h"
#include "field/schema.h"
#include "core/types.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Clock  = std::chrono::steady_clock;

// ─────────────────────────────────────────────────────────────────────────────
// JSON 单行字段提取（处理 \" \n \t \\ 转义，不依赖第三方库）
// ─────────────────────────────────────────────────────────────────────────────
static std::string extractJsonString(const std::string& line,
                                     const std::string& key)
{
    std::string pat1 = "\"" + key + "\": \"";
    std::string pat2 = "\"" + key + "\":\"";
    size_t pos = line.find(pat1);
    size_t skip = pat1.size();
    if (pos == std::string::npos) {
        pos = line.find(pat2);
        skip = pat2.size();
    }
    if (pos == std::string::npos) return "";
    pos += skip;

    std::string result;
    result.reserve(512);
    while (pos < line.size()) {
        char c = line[pos];
        if (c == '\\' && pos + 1 < line.size()) {
            char n = line[pos + 1];
            switch (n) {
                case '"':  result += '"';  break;
                case 'n':  result += '\n'; break;
                case 't':  result += '\t'; break;
                case 'r':  result += '\r'; break;
                case '\\': result += '\\'; break;
                default:   result += n;    break;
            }
            pos += 2;
        } else if (c == '"') {
            break;
        } else {
            result += c;
            ++pos;
        }
    }
    return result;
}

// 从 JSON 行中提取无引号数字（先尝试带引号，再尝试不带引号）
static int64_t extractJsonInt64(const std::string& line, const std::string& key) {
    // 先尝试带引号的字符串数字（"id": "12345"）
    std::string quoted = extractJsonString(line, key);
    if (!quoted.empty()) {
        try { return std::stoll(quoted); } catch (...) {}
    }
    // 再找裸数字（"pubtime": 1700000000）
    for (const char* pat_sfx : {": ", ":"}) {
        std::string pat = "\"" + key + "\"" + pat_sfx;
        size_t pos = line.find(pat);
        if (pos == std::string::npos) continue;
        pos += pat.size();
        while (pos < line.size() && line[pos] == ' ') ++pos;
        if (pos >= line.size()) continue;
        try {
            size_t consumed = 0;
            int64_t v = std::stoll(line.substr(pos), &consumed);
            if (consumed > 0) return v;
        } catch (...) {}
    }
    return 0;
}

static float extractJsonFloat32(const std::string& line, const std::string& key) {
    std::string quoted = extractJsonString(line, key);
    if (!quoted.empty()) {
        try { return std::stof(quoted); } catch (...) {}
    }
    for (const char* pat_sfx : {": ", ":"}) {
        std::string pat = "\"" + key + "\"" + pat_sfx;
        size_t pos = line.find(pat);
        if (pos == std::string::npos) continue;
        pos += pat.size();
        while (pos < line.size() && line[pos] == ' ') ++pos;
        if (pos >= line.size()) continue;
        try {
            size_t consumed = 0;
            float v = std::stof(line.substr(pos), &consumed);
            if (consumed > 0) return v;
        } catch (...) {}
    }
    return 0.0f;
}

// Wiki JSONL 与 Schema 字段名的特殊映射（其余字段名即 JSON key）
static std::string wikiJsonKey(const std::string& field_name) {
    if (field_name == "body")   return "text";  // wiki 把正文叫 "text"
    if (field_name == "source") return "url";   // wiki 把来源叫 "url"
    return field_name;
}

// 按 Schema 逐字段从 JSON 行提取值并填充 Document（Schema 驱动，无硬编码字段名）
static void populateDocument(ii::Document& doc,
                              const std::string& line,
                              const ii::Schema&  schema)
{
    for (const auto& fs : schema.fields) {
        std::string jkey = wikiJsonKey(fs.name);

        if (fs.type == ii::FieldType::Text || fs.type == ii::FieldType::Keyword) {
            std::string val = extractJsonString(line, jkey);
            if (!val.empty()) doc.set(fs.name, std::move(val));

        } else if (fs.type == ii::FieldType::Int64) {
            doc.set(fs.name, extractJsonInt64(line, jkey));

        } else if (fs.type == ii::FieldType::Float32) {
            doc.set(fs.name, extractJsonFloat32(line, jkey));
        }
    }
}

// 判断文档是否有可索引的文本内容（跳过完全为空的文档）
static bool hasIndexableContent(const std::string& line, const ii::Schema& schema) {
    for (const auto& fs : schema.fields) {
        if (fs.index == ii::IndexOption::None) continue;
        if (fs.type != ii::FieldType::Text && fs.type != ii::FieldType::Keyword) continue;
        if (!extractJsonString(line, wikiJsonKey(fs.name)).empty()) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// 参数解析
// ─────────────────────────────────────────────────────────────────────────────
struct Args {
    std::string input_dir;
    std::string output_dir  = "./wiki_index";
    std::string schema_path;                   // --schema（可选）
    int         workers     = 1;               // --workers（1 = 单线程）
    float       ram_mb      = 128.0f;          // 单线程总 buffer / 多线程 per-worker flush 阈值
    uint64_t    limit       = UINT64_MAX;
    uint32_t    top         = 50;
    bool        verbose     = false;
};

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --input <wiki_dir>"
                 " [--output <dir>]"
                 " [--schema <schema.json>]"
                 " [--workers <N>]"
                 " [--ram <MB>]"
                 " [--limit <N>]"
                 " [--top <N>]"
                 " [--verbose]\n";
    std::exit(1);
}

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];
        if      (flag == "--input"   && i+1 < argc) { a.input_dir   = argv[++i]; }
        else if (flag == "--output"  && i+1 < argc) { a.output_dir  = argv[++i]; }
        else if (flag == "--schema"  && i+1 < argc) { a.schema_path = argv[++i]; }
        else if (flag == "--workers" && i+1 < argc) { a.workers     = std::stoi(argv[++i]); }
        else if (flag == "--ram"     && i+1 < argc) { a.ram_mb      = std::stof(argv[++i]); }
        else if (flag == "--limit"   && i+1 < argc) { a.limit       = std::stoull(argv[++i]); }
        else if (flag == "--top"     && i+1 < argc) { a.top         = std::stoul(argv[++i]); }
        else if (flag == "--verbose")                { a.verbose     = true; }
        else { usage(argv[0]); }
    }
    if (a.input_dir.empty()) usage(argv[0]);
    if (a.workers < 1) a.workers = 1;
    return a;
}

// ─────────────────────────────────────────────────────────────────────────────
// Schema 加载 + 打印摘要
// ─────────────────────────────────────────────────────────────────────────────
static ii::Schema loadSchema(const Args& args) {
    if (!args.schema_path.empty()) {
        std::cout << "[Schema] Loading from file : " << args.schema_path << "\n";
        return ii::Schema::fromJson(args.schema_path);
    }
    // 优先加载输出目录中已有的 schema.json（续建场景）
    std::string existing = args.output_dir + "/schema.json";
    if (fs::exists(existing)) {
        std::cout << "[Schema] Loading from index: " << existing << "\n";
        return ii::Schema::fromJson(existing);
    }
    std::cout << "[Schema] No schema specified, using defaultSchema\n";
    return ii::Schema::defaultSchema();
}

static void printSchema(const ii::Schema& schema) {
    std::cout << "[Schema] " << schema.fields.size() << " field(s):\n";
    for (const auto& f : schema.fields) {
        std::cout << "  " << std::left << std::setw(14) << f.name
                  << " type="  << std::setw(8)  << ii::fieldTypeToStr(f.type)
                  << " index=" << std::setw(18) << ii::indexOptionToStr(f.index);
        if (f.stored) std::cout << " stored";
        if (f.fast)   std::cout << " fast";
        std::cout << "  (json_key=\"" << wikiJsonKey(f.name) << "\")\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildResult：buildIndex 返回的构建结果（含 per-segment 统计）
// ─────────────────────────────────────────────────────────────────────────────
struct BuildResult {
    uint64_t doc_count  = 0;
    uint64_t skip_count = 0;
    double   elapsed_s  = 0.0;
    std::vector<ii::SegmentWriteStats> seg_stats;
    std::vector<ii::FFWriteStats>      ff_stats;
};

// ─────────────────────────────────────────────────────────────────────────────
// printBuildSummary：构建完成后打印跨 segment 汇总表
// ─────────────────────────────────────────────────────────────────────────────
static void printBuildSummary(const BuildResult& r) {
    if (r.seg_stats.empty()) return;

    auto kb = [](uint64_t b)  { return static_cast<double>(b) / 1024.0; };
    auto ms = [](uint64_t us) { return static_cast<double>(us) / 1000.0; };

    // 累计值
    uint64_t tot_docs = 0, tot_terms = 0;
    uint64_t tot_pl   = 0, tot_skip  = 0;
    uint64_t tot_tim  = 0, tot_doc   = 0, tot_pos   = 0, tot_fdtfdx = 0;
    uint64_t tot_seg_bytes = 0, tot_ff_bytes = 0;
    uint64_t tot_tim_us = 0, tot_doc_us = 0, tot_pos_us = 0, tot_fdtfdx_us = 0, tot_ff_us = 0;

    std::cout << "\n" << std::string(80, '=') << "\n";
    std::cout << "  Segment Build Summary  (" << r.seg_stats.size() << " segment(s))\n";
    std::cout << std::string(80, '=') << "\n";

    // 表头
    std::cout << std::left
              << std::setw(6)  << "SegID"
              << std::setw(7)  << "Docs"
              << std::setw(7)  << "Terms"
              << std::setw(9)  << "PLentry"
              << std::setw(8)  << "Skip"
              << std::setw(9)  << ".tim KB"
              << std::setw(9)  << ".doc KB"
              << std::setw(9)  << ".pos KB"
              << std::setw(11) << ".fdt+fdx KB"
              << std::setw(9)  << "FF KB"
              << std::setw(9)  << "Total KB"
              << "Time ms\n";
    std::cout << std::string(80, '-') << "\n";

    for (size_t i = 0; i < r.seg_stats.size(); ++i) {
        const auto& s  = r.seg_stats[i];
        const auto& ff = r.ff_stats[i];
        uint64_t seg_total_us = s.totalSegUs() + ff.total_us;

        std::cout << std::left
                  << std::setw(6)  << ("_" + std::to_string(s.segment_id))
                  << std::setw(7)  << s.doc_count
                  << std::setw(7)  << s.term_count
                  << std::setw(9)  << s.total_pl_entries
                  << std::setw(8)  << s.total_skip_nodes
                  << std::fixed << std::setprecision(1)
                  << std::setw(9)  << kb(s.tim_bytes)
                  << std::setw(9)  << kb(s.doc_bytes)
                  << std::setw(9)  << kb(s.pos_bytes)
                  << std::setw(11) << kb(s.fdt_bytes + s.fdx_bytes)
                  << std::setw(9)  << kb(ff.total_bytes)
                  << std::setw(9)  << kb(s.totalSegBytes() + ff.total_bytes)
                  << ms(seg_total_us) << "\n";

        tot_docs      += s.doc_count;
        tot_terms     += s.term_count;
        tot_pl        += s.total_pl_entries;
        tot_skip      += s.total_skip_nodes;
        tot_tim       += s.tim_bytes;
        tot_doc       += s.doc_bytes;
        tot_pos       += s.pos_bytes;
        tot_fdtfdx    += s.fdt_bytes + s.fdx_bytes;
        tot_ff_bytes  += ff.total_bytes;
        tot_seg_bytes += s.totalSegBytes();
        tot_tim_us    += s.tim_us;
        tot_doc_us    += s.doc_us;
        tot_pos_us    += s.pos_us;
        tot_fdtfdx_us += s.fdt_fdx_us;
        tot_ff_us     += ff.total_us;
    }

    std::cout << std::string(80, '-') << "\n";
    std::cout << std::left
              << std::setw(6)  << "Total"
              << std::setw(7)  << tot_docs
              << std::setw(7)  << tot_terms
              << std::setw(9)  << tot_pl
              << std::setw(8)  << tot_skip
              << std::fixed << std::setprecision(1)
              << std::setw(9)  << kb(tot_tim)
              << std::setw(9)  << kb(tot_doc)
              << std::setw(9)  << kb(tot_pos)
              << std::setw(11) << kb(tot_fdtfdx)
              << std::setw(9)  << kb(tot_ff_bytes)
              << std::setw(9)  << kb(tot_seg_bytes + tot_ff_bytes)
              << ms(tot_tim_us + tot_doc_us + tot_pos_us + tot_fdtfdx_us + tot_ff_us) << "\n";

    std::cout << std::string(80, '=') << "\n";

    // 写文件时间明细（总计）
    std::cout << "  Write time breakdown (total across all segments):\n";
    std::cout << "    .tim      : " << std::setw(8) << ms(tot_tim_us)    << " ms\n";
    std::cout << "    .doc      : " << std::setw(8) << ms(tot_doc_us)    << " ms\n";
    std::cout << "    .pos      : " << std::setw(8) << ms(tot_pos_us)    << " ms\n";
    std::cout << "    .fdt+.fdx : " << std::setw(8) << ms(tot_fdtfdx_us) << " ms\n";
    std::cout << "    .ff       : " << std::setw(8) << ms(tot_ff_us)     << " ms\n";
    std::cout << "    index I/O : " << std::setw(8)
              << ms(tot_tim_us + tot_doc_us + tot_pos_us + tot_fdtfdx_us + tot_ff_us)
              << " ms  (wall time: " << std::setprecision(2) << r.elapsed_s << "s)\n";
    std::cout << std::string(80, '=') << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// collectSegmentStats：从磁盘文件尺寸重建 SegmentWriteStats/FFWriteStats
// （并行模式下 Writer 不持有写统计，由此在 commit 后按需收集）
// ─────────────────────────────────────────────────────────────────────────────
static std::pair<ii::SegmentWriteStats, ii::FFWriteStats>
collectSegmentStats(const std::string& dir, uint32_t seg_id)
{
    auto fsize = [](const std::string& p) -> uint64_t {
        std::error_code ec;
        auto sz = fs::file_size(p, ec);
        return ec ? 0 : static_cast<uint64_t>(sz);
    };
    auto fp = [&](const std::string& ext) {
        return dir + "/_" + std::to_string(seg_id) + "." + ext;
    };

    ii::SegmentWriteStats seg;
    seg.segment_id = seg_id;

    // doc/term 数量从 SegmentReader 读取
    ii::SegmentReader reader(dir, seg_id);
    seg.doc_count  = reader.docCount();
    seg.term_count = reader.termCount();

    // 各倒排文件尺寸（按字段累加）
    for (const auto& field : reader.indexedFieldNames()) {
        seg.tim_bytes += fsize(fp("tim_" + field));
        seg.doc_bytes += fsize(fp("doc_" + field));
        seg.pos_bytes += fsize(fp("pos_" + field));
    }
    seg.fdt_bytes = fsize(fp("fdt"));
    seg.fdx_bytes = fsize(fp("fdx"));
    seg.liv_bytes = fsize(fp("liv"));
    seg.si_bytes  = fsize(fp("si"));

    // FastField 文件：扫描 _N.ff_* 前缀
    ii::FFWriteStats ff;
    const std::string ff_prefix = "_" + std::to_string(seg_id) + ".ff_";
    for (const auto& entry : fs::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > ff_prefix.size() &&
            name.substr(0, ff_prefix.size()) == ff_prefix) {
            uint64_t sz = fsize(entry.path().string());
            ff.file_bytes[name.substr(ff_prefix.size())] = sz;
            ff.total_bytes += sz;
        }
    }

    return {seg, ff};
}

// ─────────────────────────────────────────────────────────────────────────────
// 共用：收集输入文件列表 + 打印开头信息
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<fs::path> collectInputFiles(const Args& args) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(args.input_dir))
        if (entry.is_regular_file())
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    return files;
}

static void printBuildHeader(const Args& args, size_t n_files) {
    std::cout << "[Build] Found " << n_files << " file(s) in " << args.input_dir << "\n";
    if (args.workers > 1)
        std::cout << "[Build] Mode       : parallel  workers=" << args.workers
                  << "  ram_per_worker=" << args.ram_mb << " MB\n";
    else
        std::cout << "[Build] Mode       : single-thread  ram_buffer=" << args.ram_mb << " MB\n";
    std::cout << "[Build] Output dir : " << args.output_dir << "\n";
    if (args.limit != UINT64_MAX)
        std::cout << "[Build] Doc limit  : " << args.limit << "\n";
    std::cout << std::string(60, '-') << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// 阶段一A：单线程构建（IndexWriter，stats 完整）
// ─────────────────────────────────────────────────────────────────────────────
static BuildResult buildIndexSingle(const Args& args,
                                    const std::vector<fs::path>& files,
                                    const ii::Schema& schema)
{
    ii::IndexWriter writer(args.output_dir, args.ram_mb, schema);

    BuildResult result;
    uint64_t& doc_count  = result.doc_count;
    uint64_t& skip_count = result.skip_count;
    uint64_t file_count  = 0;
    auto t_start = Clock::now();

    for (const auto& path : files) {
        if (doc_count >= args.limit) break;
        std::ifstream f(path);
        if (!f) { std::cerr << "[WARN] Cannot open: " << path << "\n"; continue; }

        std::string line;
        while (std::getline(f, line)) {
            if (doc_count >= args.limit) break;
            if (line.empty()) continue;
            if (!hasIndexableContent(line, schema)) { ++skip_count; continue; }

            ii::Document doc;
            doc.doc_id = static_cast<ii::DocId>(doc_count + 1);
            doc.ext_id = static_cast<uint64_t>(extractJsonInt64(line, "id"));
            populateDocument(doc, line, schema);
            writer.addDocument(doc);
            ++doc_count;

            if (doc_count % 1000 == 0) {
                auto now    = Clock::now();
                double elapsed = std::chrono::duration<double>(now - t_start).count();
                std::cout << "\r[Build] " << std::setw(8) << doc_count
                          << " docs  |  " << std::fixed << std::setprecision(0)
                          << doc_count / elapsed << " docs/s  |  "
                          << std::setprecision(1) << elapsed << "s elapsed   "
                          << std::flush;
            }
        }
        ++file_count;
    }

    std::cout << "\n[Build] Committing index...\n";
    writer.commit();

    result.elapsed_s = std::chrono::duration<double>(Clock::now() - t_start).count();
    result.seg_stats = writer.segStatsHistory();
    result.ff_stats  = writer.ffStatsHistory();

    std::cout << "[Build] Done.\n"
              << "[Build] Files processed : " << file_count << "\n"
              << "[Build] Docs indexed    : " << doc_count << "\n"
              << "[Build] Docs skipped    : " << skip_count << " (no indexable content)\n"
              << "[Build] Total time      : " << std::fixed << std::setprecision(2)
              << result.elapsed_s << "s\n"
              << "[Build] Throughput      : " << std::setprecision(0)
              << (doc_count / result.elapsed_s) << " docs/s\n";
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 阶段一B：并行构建（ParallelIndexWriter，stats 从磁盘重建）
// ─────────────────────────────────────────────────────────────────────────────
static BuildResult buildIndexParallel(const Args& args,
                                      const std::vector<fs::path>& files,
                                      const ii::Schema& schema)
{
    ii::ParallelIndexWriter writer(args.output_dir, args.workers, args.ram_mb, schema);

    BuildResult result;
    uint64_t& doc_count  = result.doc_count;
    uint64_t& skip_count = result.skip_count;
    uint64_t file_count  = 0;
    auto t_start = Clock::now();

    for (const auto& path : files) {
        if (doc_count >= args.limit) break;
        std::ifstream f(path);
        if (!f) { std::cerr << "[WARN] Cannot open: " << path << "\n"; continue; }

        std::string line;
        while (std::getline(f, line)) {
            if (doc_count >= args.limit) break;
            if (line.empty()) continue;
            if (!hasIndexableContent(line, schema)) { ++skip_count; continue; }

            ii::Document doc;
            // doc.doc_id 由 ParallelIndexWriter 内部原子分配，无需手动设置
            doc.ext_id = static_cast<uint64_t>(extractJsonInt64(line, "id"));
            populateDocument(doc, line, schema);
            writer.addDocument(std::move(doc));
            ++doc_count;

            if (doc_count % 1000 == 0) {
                auto now     = Clock::now();
                double elapsed = std::chrono::duration<double>(now - t_start).count();
                std::cout << "\r[Build] " << std::setw(8) << doc_count
                          << " docs  |  " << std::fixed << std::setprecision(0)
                          << doc_count / elapsed << " docs/s  |  "
                          << std::setprecision(1) << elapsed << "s elapsed   "
                          << std::flush;
            }
        }
        ++file_count;
    }

    std::cout << "\n[Build] Committing index...\n";
    writer.commit();

    result.elapsed_s = std::chrono::duration<double>(Clock::now() - t_start).count();

    // commit 后从磁盘重建 stats（含文件大小，不含写耗时）
    for (uint32_t sid : writer.segmentIds()) {
        auto [seg, ff] = collectSegmentStats(args.output_dir, sid);
        result.seg_stats.push_back(seg);
        result.ff_stats.push_back(ff);
    }

    std::cout << "[Build] Done.\n"
              << "[Build] Files processed : " << file_count << "\n"
              << "[Build] Docs indexed    : " << doc_count << "\n"
              << "[Build] Docs skipped    : " << skip_count << " (no indexable content)\n"
              << "[Build] Total time      : " << std::fixed << std::setprecision(2)
              << result.elapsed_s << "s\n"
              << "[Build] Throughput      : " << std::setprecision(0)
              << (doc_count / result.elapsed_s) << " docs/s\n";
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 阶段一：构建索引（dispatcher）
// ─────────────────────────────────────────────────────────────────────────────
static BuildResult buildIndex(const Args& args) {
    auto files = collectInputFiles(args);
    printBuildHeader(args, files.size());

    ii::Schema schema = loadSchema(args);
    printSchema(schema);
    std::cout << std::string(60, '-') << "\n";

    if (args.workers <= 1)
        return buildIndexSingle(args, files, schema);
    else
        return buildIndexParallel(args, files, schema);
}

// ─────────────────────────────────────────────────────────────────────────────
// 阶段二：打印 Term Posting 详情
// ─────────────────────────────────────────────────────────────────────────────
static void printPostings(const Args& args, bool verbose) {
    std::vector<uint32_t> seg_ids;
    for (const auto& entry : fs::directory_iterator(args.output_dir)) {
        auto name = entry.path().filename().string();
        if (name.size() > 3 && name[0] == '_' &&
            name.substr(name.size() - 3) == ".si") {
            try {
                seg_ids.push_back(std::stoul(name.substr(1, name.size() - 4)));
            } catch (...) {}
        }
    }
    std::sort(seg_ids.begin(), seg_ids.end());

    if (seg_ids.empty()) {
        std::cerr << "[Stats] No segments found in " << args.output_dir << "\n";
        return;
    }

    struct GlobalMeta {
        uint32_t df  = 0;
        uint32_t ttf = 0;
        float    ub  = 0.0f;
        uint32_t seg_id = 0;
    };
    std::map<std::string, GlobalMeta> global_dict;

    uint64_t total_docs  = 0;
    uint64_t total_terms = 0;

    std::vector<std::unique_ptr<ii::SegmentReader>> readers;
    for (uint32_t sid : seg_ids) {
        auto reader = std::make_unique<ii::SegmentReader>(args.output_dir, sid);
        total_docs  += reader->docCount();
        total_terms += reader->termCount();

        for (const auto& [term, meta] : reader->termDict()) {
            auto& g = global_dict[term];
            g.df  += meta.doc_freq;
            g.ttf += meta.total_term_freq;
            if (meta.upper_bound > g.ub) {
                g.ub     = meta.upper_bound;
                g.seg_id = sid;
            }
        }
        readers.push_back(std::move(reader));
    }

    std::vector<std::pair<std::string, GlobalMeta>> sorted_terms(
        global_dict.begin(), global_dict.end());
    std::sort(sorted_terms.begin(), sorted_terms.end(),
        [](const auto& a, const auto& b) { return a.second.df > b.second.df; });

    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "  Index Statistics\n";
    std::cout << std::string(70, '=') << "\n";
    std::cout << "  Segments         : " << seg_ids.size() << "\n";
    std::cout << "  Total docs       : " << total_docs << "\n";
    std::cout << "  Unique terms     : " << global_dict.size() << "\n";
    std::cout << "  Total term occurrences (ttf sum): " << [&]{
        uint64_t s = 0;
        for (auto& [t,m] : global_dict) s += m.ttf;
        return s;
    }() << "\n";

    uint32_t top = std::min((uint32_t)sorted_terms.size(), args.top);

    std::cout << "\n" << std::string(70, '-') << "\n";
    std::cout << "  Top " << top << " terms by document frequency (with posting sample)\n";
    std::cout << std::string(70, '-') << "\n";
    std::cout << std::left
              << std::setw(6)  << "Rank"
              << std::setw(28) << "Term"
              << std::setw(8)  << "df"
              << std::setw(10) << "ttf"
              << std::setw(8)  << "UB"
              << "Posting sample (first 15 doc_ids)\n";
    std::cout << std::string(70, '-') << "\n";

    for (uint32_t i = 0; i < top; ++i) {
        const auto& [term, meta] = sorted_terms[i];

        ii::SegmentReader* seg = nullptr;
        for (auto& r : readers) {
            if (r->segmentId() == meta.seg_id) { seg = r.get(); break; }
        }

        std::string sample_str;
        if (seg) {
            auto docs = seg->readPostingList(term);
            sample_str = "[";
            uint32_t show = std::min((uint32_t)docs.size(), 15u);
            for (uint32_t j = 0; j < show; ++j) {
                if (j) sample_str += ", ";
                sample_str += std::to_string(docs[j]);
            }
            if (docs.size() > 15) sample_str += ", ...";
            sample_str += "]";
        }

        std::string display_term = term.size() > 25
                                   ? term.substr(0, 22) + "..."
                                   : term;

        std::cout << std::left
                  << std::setw(6)  << (i + 1)
                  << std::setw(28) << display_term
                  << std::setw(8)  << meta.df
                  << std::setw(10) << meta.ttf
                  << std::setw(8)  << std::fixed << std::setprecision(3) << meta.ub
                  << sample_str << "\n";
    }

    uint64_t remaining = sorted_terms.size() - top;
    if (remaining > 0 && verbose) {
        std::cout << "\n" << std::string(70, '-') << "\n";
        std::cout << "  All remaining terms (" << remaining << ")  — df / ttf / UB only\n";
        std::cout << std::string(70, '-') << "\n";
        std::cout << std::left
                  << std::setw(28) << "Term"
                  << std::setw(8)  << "df"
                  << std::setw(10) << "ttf"
                  << "UB\n";
        std::cout << std::string(46, '-') << "\n";

        for (uint32_t i = top; i < (uint32_t)sorted_terms.size(); ++i) {
            const auto& [term, meta] = sorted_terms[i];
            std::string display_term = term.size() > 25
                                       ? term.substr(0, 22) + "..."
                                       : term;
            std::cout << std::left
                      << std::setw(28) << display_term
                      << std::setw(8)  << meta.df
                      << std::setw(10) << meta.ttf
                      << std::fixed << std::setprecision(3) << meta.ub << "\n";
        }
    } else if (remaining > 0) {
        std::cout << "\n  (" << remaining
                  << " more terms omitted — use --verbose to show all)\n";
    }

    std::cout << "\n" << std::string(70, '=') << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    fs::create_directories(args.output_dir);

    BuildResult result = buildIndex(args);
    printBuildSummary(result);
    printPostings(args, args.verbose);

    return 0;
}
