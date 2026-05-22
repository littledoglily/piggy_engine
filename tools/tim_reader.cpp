// tools/tim_reader.cpp
//
// .tim 文件解析工具，用于调试索引词典
//
// 用法（单次查询）:
//   tim_reader --file <path.tim> --term <term>
//
// 用法（只看 term 数量）:
//   tim_reader --file <path.tim> --count
//
// 用法（交互模式，不传 --term/--count）:
//   tim_reader --file <path.tim>
//
// 交互命令:
//   <term>         查找 term，打印全部字段信息
//   :count         显示当前文件的 term 总数
//   :list [N]      列出前 N 个 term（默认 20）
//   :quit / :q     退出

#include <cstring>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// .tim 文件格式（per-field 版本）：
//   4B  term_count
//   for each term (字母序):
//     4B+N  term string (length-prefixed)
//     4B    doc_freq
//     4B    total_term_freq
//     8B    posting_offset
//     8B    skip_offset
//     8B    pos_offset
//     4B    upper_bound  (float, BM25 tf-norm 上界)
//     8B    tf_data_offset
// ─────────────────────────────────────────────────────────────────────────────

struct TermEntry {
    std::string term;
    uint32_t    doc_freq        = 0;
    uint32_t    total_term_freq = 0;
    uint64_t    posting_offset  = 0;
    uint64_t    skip_offset     = 0;
    uint64_t    pos_offset      = 0;
    float       upper_bound     = 0.f;
    uint64_t    tf_data_offset  = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// 二进制读取辅助
// ─────────────────────────────────────────────────────────────────────────────

static bool readU32(std::ifstream& f, uint32_t& out) {
    return static_cast<bool>(f.read(reinterpret_cast<char*>(&out), 4));
}

static bool readU64(std::ifstream& f, uint64_t& out) {
    return static_cast<bool>(f.read(reinterpret_cast<char*>(&out), 8));
}

static bool readF32(std::ifstream& f, float& out) {
    return static_cast<bool>(f.read(reinterpret_cast<char*>(&out), 4));
}

static bool readStr(std::ifstream& f, std::string& out) {
    uint32_t len = 0;
    if (!readU32(f, len)) return false;
    out.resize(len);
    return static_cast<bool>(f.read(&out[0], len));
}

// ─────────────────────────────────────────────────────────────────────────────
// .tim 加载
// ─────────────────────────────────────────────────────────────────────────────

struct TimFile {
    uint32_t                                  term_count = 0;
    std::vector<std::string>                  order;       // 保留原始顺序
    std::unordered_map<std::string, TermEntry> dict;
};

static bool loadTim(const std::string& path, TimFile& out, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open file: " + path; return false; }

    if (!readU32(f, out.term_count)) {
        err = "failed to read term_count"; return false;
    }

    out.order.reserve(out.term_count);
    out.dict.reserve(out.term_count);

    for (uint32_t i = 0; i < out.term_count; ++i) {
        TermEntry e;
        if (!readStr(f, e.term))               { err = "read term failed at entry " + std::to_string(i); return false; }
        if (!readU32(f, e.doc_freq))           { err = "read doc_freq failed";       return false; }
        if (!readU32(f, e.total_term_freq))    { err = "read total_term_freq failed"; return false; }
        if (!readU64(f, e.posting_offset))     { err = "read posting_offset failed"; return false; }
        if (!readU64(f, e.skip_offset))        { err = "read skip_offset failed";    return false; }
        if (!readU64(f, e.pos_offset))         { err = "read pos_offset failed";     return false; }
        if (!readF32(f, e.upper_bound))        { err = "read upper_bound failed";    return false; }
        // tf_data_offset 在旧格式中不存在，读到 EOF 时跳过
        if (f.peek() != std::char_traits<char>::eof())
            readU64(f, e.tf_data_offset);

        out.order.push_back(e.term);
        out.dict.emplace(e.term, e);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 格式化打印
// ─────────────────────────────────────────────────────────────────────────────

static void printEntry(const TermEntry& e) {
    std::cout << "  term            : " << e.term            << "\n"
              << "  doc_freq        : " << e.doc_freq        << "\n"
              << "  total_term_freq : " << e.total_term_freq << "\n"
              << "  posting_offset  : 0x" << std::hex << std::setw(12) << std::setfill('0')
                                          << e.posting_offset  << std::dec << "\n"
              << "  skip_offset     : 0x" << std::hex << std::setw(12) << std::setfill('0')
                                          << e.skip_offset     << std::dec << "\n"
              << "  pos_offset      : 0x" << std::hex << std::setw(12) << std::setfill('0')
                                          << e.pos_offset      << std::dec << "\n"
              << "  upper_bound     : " << std::fixed << std::setprecision(6)
                                        << e.upper_bound      << "\n"
              << "  tf_data_offset  : 0x" << std::hex << std::setw(12) << std::setfill('0')
                                          << e.tf_data_offset  << std::dec << "\n";
}

static void printCount(const TimFile& tim) {
    std::cout << "term_count: " << tim.term_count << "\n";
}

static void listTerms(const TimFile& tim, uint32_t n) {
    uint32_t limit = std::min(n, static_cast<uint32_t>(tim.order.size()));
    for (uint32_t i = 0; i < limit; ++i)
        std::cout << "  [" << std::setw(6) << i << "]  " << tim.order[i] << "\n";
    if (limit < tim.term_count)
        std::cout << "  ... (" << tim.term_count - limit << " more)\n";
}

static void lookup(const TimFile& tim, const std::string& term) {
    auto it = tim.dict.find(term);
    if (it == tim.dict.end()) {
        std::cout << "[not found] \"" << term << "\"\n";
    } else {
        std::cout << "[found]\n";
        printEntry(it->second);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 交互循环
// ─────────────────────────────────────────────────────────────────────────────

static void interactiveLoop(const TimFile& tim) {
    std::cout << "Loaded " << tim.term_count << " terms. "
              << "Commands: <term>  :count  :list [N]  :quit\n";
    std::string line;
    while (true) {
        std::cout << "tim> " << std::flush;
        if (!std::getline(std::cin, line)) break;

        // strip leading/trailing whitespace
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        line = line.substr(s, e - s + 1);
        if (line.empty()) continue;

        if (line == ":quit" || line == ":q") {
            break;
        } else if (line == ":count") {
            printCount(tim);
        } else if (line.rfind(":list", 0) == 0) {
            uint32_t n = 20;
            if (line.size() > 5) {
                try { n = static_cast<uint32_t>(std::stoul(line.substr(6))); }
                catch (...) {}
            }
            listTerms(tim, n);
        } else {
            lookup(tim, line);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

static void usage() {
    std::cerr <<
        "Usage:\n"
        "  tim_reader --file <path.tim> --term <term>   # single lookup\n"
        "  tim_reader --file <path.tim> --count         # term count only\n"
        "  tim_reader --file <path.tim>                 # interactive\n"
        "  tim_reader --file <path.tim> --list [N]      # list first N terms\n";
}

int main(int argc, char* argv[]) {
    std::string file_path;
    std::string term_arg;
    bool        do_count = false;
    bool        do_list  = false;
    uint32_t    list_n   = 20;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--file" && i + 1 < argc) {
            file_path = argv[++i];
        } else if (a == "--term" && i + 1 < argc) {
            term_arg = argv[++i];
        } else if (a == "--count") {
            do_count = true;
        } else if (a == "--list") {
            do_list = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                try { list_n = static_cast<uint32_t>(std::stoul(argv[++i])); }
                catch (...) {}
            }
        } else {
            std::cerr << "Unknown argument: " << a << "\n";
            usage();
            return 1;
        }
    }

    if (file_path.empty()) {
        std::cerr << "Error: --file is required\n";
        usage();
        return 1;
    }

    TimFile tim;
    std::string err;
    if (!loadTim(file_path, tim, err)) {
        std::cerr << "Error loading tim file: " << err << "\n";
        return 1;
    }

    if (do_count) {
        printCount(tim);
    } else if (!term_arg.empty()) {
        lookup(tim, term_arg);
    } else if (do_list) {
        printCount(tim);
        listTerms(tim, list_n);
    } else {
        interactiveLoop(tim);
    }

    return 0;
}
