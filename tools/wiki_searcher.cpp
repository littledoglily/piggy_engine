// tools/wiki_searcher.cpp
//
// 用法（单次查询）:
//   wiki_searcher --index <index_dir> --query "python language" [options]
//
// 用法（交互模式，不传 --query）:
//   wiki_searcher --index <index_dir>
//
// Options:
//   --index  <path>   索引目录（wiki_indexer 构建的输出，必填）
//   --query  <text>   查询词，空格分隔多个词（不传则进入交互模式）
//   --mode   AND|OR   查询模式（默认 OR）
//   --top    <N>      返回前 N 个结果（默认 10）

#include "query/index_searcher.h"
#include "types.h"

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
};

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --index <dir> [--query <text>] [--mode AND|OR] [--top N]\n";
    std::exit(1);
}

static Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string f = argv[i];
        if      (f == "--index" && i+1 < argc) { a.index_dir = argv[++i]; }
        else if (f == "--query" && i+1 < argc) { a.query     = argv[++i]; }
        else if (f == "--top"   && i+1 < argc) { a.top       = std::stoi(argv[++i]); }
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

    std::cout << std::string(72, '-') << "\n";
    std::cout << std::left
              << std::setw(5)  << "Rank"
              << std::setw(10) << "DocID"
              << std::setw(9)  << "Score"
              << "Title (Wiki Article)\n";
    std::cout << std::string(72, '-') << "\n";

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];

        // 截断过长标题（防止换行破坏对齐）
        std::string title = r.title;
        if (title.size() > 48) title = title.substr(0, 45) + "...";

        std::cout << std::left
                  << std::setw(5)  << (i + 1)
                  << std::setw(10) << r.doc_id
                  << std::fixed << std::setprecision(4)
                  << std::setw(9)  << r.score
                  << title << "\n";
    }
    std::cout << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    std::cout << "[Searcher] Loading index from: " << args.index_dir << "\n";
    ii::IndexSearcher searcher(args.index_dir);
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
        std::cout << "\nInteractive mode — type a query and press Enter. "
                     "Ctrl-D or 'quit' to exit.\n"
                  << "Mode: " << (args.mode == ii::QueryMode::AND ? "AND" : "OR")
                  << "  Top: " << args.top
                  << "  (change with --mode / --top at startup)\n\n";

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
