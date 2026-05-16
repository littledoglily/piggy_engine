// tools/wiki_indexer.cpp
//
// 用法:
//   wiki_indexer --input <wiki_dir> [options]
//
// Options:
//   --input   <path>  Wiki JSONL 数据目录（递归遍历，必填）
//   --output  <path>  索引输出目录（默认 ./wiki_index）
//   --ram     <MB>    IndexWriter RAM buffer（默认 128）
//   --limit   <N>     最多索引 N 篇文档（默认不限）
//   --top     <N>     打印 posting 详情的 term 数（默认 50，按 df 降序）
//   --verbose         同时打印 top-N 之外所有 term 的 df/ttf/UB（可能数百万行，建议重定向到文件）
//
// 输入格式: JSON Lines，每行一个 JSON 对象，字段 id / title / text
// 输出: 构建完成后打印每个 term 的 df / ttf / UB，并对 top-N 展示 posting list 样本

#include "core/index_writer.h"
#include "segment/segment_reader.h"
#include "types.h"

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
    // 匹配 "key": " 或 "key":"
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

static uint64_t extractJsonId(const std::string& line) {
    std::string val = extractJsonString(line, "id");
    if (val.empty()) return 0;
    try { return std::stoull(val); }
    catch (...) { return 0; }
}

// ─────────────────────────────────────────────────────────────────────────────
// 参数解析
// ─────────────────────────────────────────────────────────────────────────────
struct Args {
    std::string input_dir;
    std::string output_dir = "./wiki_index";
    float       ram_mb     = 128.0f;
    uint64_t    limit      = UINT64_MAX;
    uint32_t    top        = 50;
    bool        verbose    = false;
};

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --input <wiki_dir> [--output <dir>] [--ram <MB>]"
                 " [--limit <N>] [--top <N>]\n";
    std::exit(1);
}

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string flag = argv[i];
        if (flag == "--input"  && i+1 < argc) { a.input_dir  = argv[++i]; }
        else if (flag == "--output" && i+1 < argc) { a.output_dir = argv[++i]; }
        else if (flag == "--ram"    && i+1 < argc) { a.ram_mb     = std::stof(argv[++i]); }
        else if (flag == "--limit"  && i+1 < argc) { a.limit      = std::stoull(argv[++i]); }
        else if (flag == "--top"     && i+1 < argc) { a.top     = std::stoul(argv[++i]); }
        else if (flag == "--verbose")              { a.verbose = true; }
        else { usage(argv[0]); }
    }
    if (a.input_dir.empty()) usage(argv[0]);
    return a;
}

// ─────────────────────────────────────────────────────────────────────────────
// 阶段一：构建索引
// ─────────────────────────────────────────────────────────────────────────────
static uint64_t buildIndex(const Args& args) {
    // 收集所有文件路径并排序（保证确定性顺序）
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(args.input_dir)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::cout << "[Build] Found " << files.size() << " file(s) in "
              << args.input_dir << "\n";
    std::cout << "[Build] Output dir: " << args.output_dir
              << "  RAM buffer: " << args.ram_mb << " MB\n";
    if (args.limit != UINT64_MAX)
        std::cout << "[Build] Doc limit: " << args.limit << "\n";
    std::cout << std::string(60, '-') << "\n";

    ii::IndexWriter writer(args.output_dir, args.ram_mb);

    uint64_t doc_count = 0;
    uint64_t skip_count = 0;  // 空 text 跳过
    uint64_t file_count = 0;
    auto t_start = Clock::now();
    auto t_last  = t_start;

    for (const auto& path : files) {
        if (doc_count >= args.limit) break;

        std::ifstream f(path);
        if (!f) {
            std::cerr << "[WARN] Cannot open: " << path << "\n";
            continue;
        }

        std::string line;
        while (std::getline(f, line)) {
            if (doc_count >= args.limit) break;
            if (line.empty()) continue;

            std::string text = extractJsonString(line, "text");
            if (text.empty()) { ++skip_count; continue; }

            ii::Document doc;
            doc.doc_id = static_cast<ii::DocId>(doc_count + 1);  // 1-indexed
            doc.ext_id = extractJsonId(line);                     // wiki numeric id
            doc.source = extractJsonString(line, "url");          // wiki URL
            doc.title  = extractJsonString(line, "title");
            doc.body   = std::move(text);

            writer.addDocument(doc);
            ++doc_count;

            // 每 1000 篇打印一次进度
            if (doc_count % 1000 == 0) {
                auto now = Clock::now();
                double elapsed = std::chrono::duration<double>(now - t_start).count();
                double rate    = doc_count / elapsed;
                double since   = std::chrono::duration<double>(now - t_last).count();
                std::cout << "\r[Build] " << std::setw(8) << doc_count
                          << " docs  |  " << std::fixed << std::setprecision(0)
                          << rate << " docs/s  |  "
                          << std::setprecision(1) << elapsed << "s elapsed   "
                          << std::flush;
                t_last = now;
            }
        }
        ++file_count;
    }

    std::cout << "\n";
    std::cout << "[Build] Committing index...\n";
    writer.commit();

    double elapsed = std::chrono::duration<double>(Clock::now() - t_start).count();
    std::cout << "[Build] Done.\n";
    std::cout << "[Build] Files processed : " << file_count << "\n";
    std::cout << "[Build] Docs indexed    : " << doc_count << "\n";
    std::cout << "[Build] Docs skipped    : " << skip_count << " (empty text)\n";
    std::cout << "[Build] Total time      : " << std::fixed << std::setprecision(2)
              << elapsed << "s\n";
    std::cout << "[Build] Throughput      : " << std::setprecision(0)
              << (doc_count / elapsed) << " docs/s\n";

    return doc_count;
}

// ─────────────────────────────────────────────────────────────────────────────
// 阶段二：打印 Term Posting 详情
// ─────────────────────────────────────────────────────────────────────────────
static void printPostings(const Args& args, bool verbose) {
    // 收集所有 .si 文件 → 确定 segment id 列表
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

    // 汇总各 segment 的词典（term → 跨 segment 累加 df / ttf）
    struct GlobalMeta {
        uint32_t df  = 0;
        uint32_t ttf = 0;
        float    ub  = 0.0f;  // 取各 segment 的最大 UB
        uint32_t seg_id = 0;  // 该 term 所在 segment（取 df 最大的那个）
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

    // 按 df 降序排序
    std::vector<std::pair<std::string, GlobalMeta>> sorted_terms(
        global_dict.begin(), global_dict.end());
    std::sort(sorted_terms.begin(), sorted_terms.end(),
        [](const auto& a, const auto& b) { return a.second.df > b.second.df; });

    // ── 打印总体统计 ────────────────────────────────────────────────────────
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

    // ── 打印 top-N 详情 ─────────────────────────────────────────────────────
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

        // 找对应 reader（取 ub 最大的 segment）
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

        // 截断过长的 term（防止乱行）
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

    // ── 打印剩余 term 汇总表（仅 df/ttf，不读 posting list）──────────────────
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

    // 确保输出目录存在
    fs::create_directories(args.output_dir);

    // Step 1: 构建索引
    buildIndex(args);

    // Step 2: 打印 posting 统计
    printPostings(args, args.verbose);

    return 0;
}
