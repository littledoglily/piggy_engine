// tools/wiki_searcher.cpp
//
// 用法（单次查询）:
//   wiki_searcher --index <index_dir> --query "python language" [options]
//   wiki_searcher --index <index_dir> --query "body:python language" --mode AND
//
// 用法（交互模式，不传 --query）:
//   wiki_searcher --index <index_dir>
//
// Options:
//   --index  <path>   索引目录（wiki_indexer 构建的输出，必填）
//   --query  <text>   查询词（支持 field:term 语法，见下方说明）
//   --mode   AND|OR   查询模式（默认 OR）
//   --top    <N>      返回前 N 个结果（默认 10）
//   --debug           打印每个 term 的 IDF / SkipNode / UB 调试信息
//
// 查询语法：
//   裸词              在所有已索引字段中搜索
//                     AND 模式：所有字段都必须包含该词（严格）
//                     OR  模式：任意字段包含即可
//
//   field:term        只在指定字段中搜索该词
//                     例：body:python        → 仅检索 body 字段
//                         source:wikipedia  → 仅检索 source 字段
//
//   混合查询          裸词与 field:term 可以混用，空格分隔
//                     例：body:python language      → body 必须有 python，
//                                                     language 展开到所有字段
//                         body:neural source:wiki  → body 含 neural
//                                                     AND source 含 wiki
//
// 交互模式下的行内模式切换：
//   :or  <query>      临时以 OR  模式执行本次查询
//   :and <query>      临时以 AND 模式执行本次查询

#include "query/index_searcher.h"
#include "core/types.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// 参数
// ─────────────────────────────────────────────────────────────────────────────
struct Args {
    std::string   index_dir;
    std::string   query;           // 空 → 交互模式
    ii::QueryMode mode  = ii::QueryMode::OR;
    int           top   = 10;
    bool          debug = false;   // --debug：打印 IDF/SkipNode/UB 调试信息
};

static void usage(const char* prog) {
    std::cerr <<
        "Usage: " << prog << " --index <dir> [options]\n"
        "\n"
        "Options:\n"
        "  --index  <path>   索引目录（必填）\n"
        "  --query  <text>   查询词（不填则进入交互模式）\n"
        "  --mode   AND|OR   查询模式（默认 OR）\n"
        "  --top    <N>      返回前 N 条结果（默认 10）\n"
        "  --debug           打印 IDF/SkipNode/UB 调试信息\n"
        "\n"
        "查询语法：\n"
        "  python language          裸词：在所有索引字段中搜索\n"
        "  body:python              field:term：只在 body 字段中搜索\n"
        "  body:python source:wiki  多个 field:term 混用\n"
        "  body:python language     field:term 与裸词混用\n"
        "\n"
        "  OR  模式（默认）：任意字段命中即可\n"
        "  AND 模式：裸词要求所有字段都包含；field:term 只限定该字段\n"
        "\n"
        "交互模式：\n"
        "  :or  <query>      临时以 OR  模式执行\n"
        "  :and <query>      临时以 AND 模式执行\n"
        "  quit / Ctrl-D     退出\n";
    std::exit(1);
}

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string f = argv[i];
        if      (f == "--index" && i+1 < argc) { a.index_dir = argv[++i]; }
        else if (f == "--query" && i+1 < argc) { a.query     = argv[++i]; }
        else if (f == "--top"   && i+1 < argc) { a.top       = std::stoi(argv[++i]); }
        else if (f == "--debug")               { a.debug     = true; }
        else if (f == "--mode"  && i+1 < argc) {
            std::string m = argv[++i];
            if (m == "AND") a.mode = ii::QueryMode::AND;
            else if (m == "OR") a.mode = ii::QueryMode::OR;
            else { std::cerr << "Unknown mode: " << m << "\n"; usage(argv[0]); }
        }
        else { usage(argv[0]); }
    }
    if (a.index_dir.empty()) usage(argv[0]);
    return a;
}

// ─────────────────────────────────────────────────────────────────────────────
// 格式化输出
// ─────────────────────────────────────────────────────────────────────────────
static void printResults(const std::string& query,
                         const std::vector<ii::SearchResult>& results,
                         ii::QueryMode mode)
{
    std::string mode_str = (mode == ii::QueryMode::AND) ? "AND" : "OR";
    std::cout << "\n[Query] \"" << query << "\"  mode=" << mode_str
              << "  hits=" << results.size() << "\n";

    if (results.empty()) {
        std::cout << "  (no results)\n\n";
        return;
    }

    std::cout << std::string(90, '-') << "\n";
    std::cout << std::left
              << std::setw(5)  << "Rank"
              << std::setw(8)  << "DocID"
              << std::setw(12) << "WikiID"
              << std::setw(9)  << "Score"
              << "Title  |  Source\n";
    std::cout << std::string(90, '-') << "\n";

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];

        std::string title = r.title;
        if (title.size() > 30) title = title.substr(0, 27) + "...";

        std::string source = r.source;
        if (source.size() > 50) source = source.substr(0, 47) + "...";

        std::cout << std::left
                  << std::setw(5)  << (i + 1)
                  << std::setw(8)  << r.doc_id
                  << std::setw(12) << r.ext_id
                  << std::fixed << std::setprecision(4)
                  << std::setw(9)  << r.score
                  << title << "  |  " << source << "\n";
    }
    std::cout << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    std::cout << "[Searcher] Loading index from: " << args.index_dir << "\n";
    ii::IndexSearcher searcher(args.index_dir);
    if (args.debug) searcher.setDebug(true);
    std::cout << "[Searcher] Ready.\n";

    auto runQuery = [&](const std::string& q) {
        if (q.empty()) return;
        auto results = searcher.search(q, args.top, args.mode);
        printResults(q, results, args.mode);
    };

    if (!args.query.empty()) {
        // 单次查询模式
        runQuery(args.query);
    } else {
        // 交互模式
        std::cout << "\nInteractive mode — type a query and press Enter. Ctrl-D or 'quit' to exit.\n"
                  << "Mode: " << (args.mode == ii::QueryMode::AND ? "AND" : "OR")
                  << "  Top: " << args.top << "\n"
                  << "Syntax: bare term  →  all fields   |  field:term  →  specific field\n"
                  << "        :or <query> / :and <query>  →  one-shot mode switch\n\n";

        std::string line;
        while (true) {
            std::cout << ">> " << std::flush;
            if (!std::getline(std::cin, line)) break;

            // 去首尾空格
            auto l = line.find_first_not_of(" \t");
            auto r = line.find_last_not_of(" \t");
            if (l == std::string::npos) continue;
            line = line.substr(l, r - l + 1);

            if (line == "quit" || line == "exit") break;

            // 支持行内切换模式：":and query" 或 ":or query"
            if (line.size() > 4 && line.substr(0, 4) == ":or ") {
                auto old = args.mode;
                args.mode = ii::QueryMode::OR;
                runQuery(line.substr(4));
                args.mode = old;
            } else if (line.size() > 5 && line.substr(0, 5) == ":and ") {
                auto old = args.mode;
                args.mode = ii::QueryMode::AND;
                runQuery(line.substr(5));
                args.mode = old;
            } else {
                runQuery(line);
            }
        }
        std::cout << "\nBye.\n";
    }

    return 0;
}
