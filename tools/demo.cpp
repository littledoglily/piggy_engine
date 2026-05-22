// ─────────────────────────────────────────────────────────────────────────────
// main.cpp  —  演示程序
//
// 流程：
//   1. 构造 256 篇文档（模拟真实数据集）
//   2. IndexWriter 写入，触发 flush（RAM buffer 满时自动 flush）
//   3. commit 持久化
//   4. IndexSearcher 打开索引，执行 AND / OR 查询
//   5. 打印结果
// ─────────────────────────────────────────────────────────────────────────────
#include "index/index_writer.h"
#include "index/segment_merger.h"
#include "query/index_searcher.h"
#include "codec/pfor_delta.h"
#include "codec/skiplist.h"
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>

// ── 构造 256 篇示例文档 ────────────────────────────────────────────────────────

static std::vector<ii::Document> buildDocuments() {
    // 5 个模板类别，各自的词汇池
    struct Category {
        std::string name;
        std::string title_prefix;
        std::vector<std::string> keywords;
    };

    std::vector<Category> categories = {
        { "python", "Python",
          {"python","programming","tutorial","beginners","advanced","data",
           "library","function","class","object","loop","variable","syntax",
           "script","module","package","pip","virtual","environment","flask",
           "django","pandas","numpy","scipy","matplotlib","machine","learning",
           "neural","network","tensorflow","pytorch","api","rest","web","server"} },
        { "java", "Java",
          {"java","spring","boot","maven","gradle","jvm","bytecode","class",
           "interface","inheritance","polymorphism","thread","concurrency","lock",
           "synchronized","executor","stream","lambda","functional","microservice",
           "docker","kubernetes","hibernate","jpa","database","sql","jdbc","rest",
           "controller","service","repository","annotation","dependency","injection"} },
        { "ml", "Machine Learning",
          {"machine","learning","algorithm","model","training","dataset","feature",
           "label","classification","regression","clustering","neural","network",
           "deep","layer","activation","gradient","descent","backpropagation",
           "overfitting","regularization","validation","accuracy","precision",
           "recall","loss","optimizer","batch","epoch","convolution","transformer",
           "attention","bert","gpt","embedding","vector","dimensionality","reduction"} },
        { "data", "Data Science",
          {"data","science","analysis","visualization","pandas","numpy","sql",
           "database","query","aggregation","groupby","join","merge","cleaning",
           "pipeline","etl","warehouse","lake","spark","hadoop","kafka","streaming",
           "batch","processing","statistics","probability","distribution","sampling",
           "hypothesis","testing","correlation","regression","dashboard","report"} },
        { "web", "Web Development",
          {"web","development","html","css","javascript","typescript","react",
           "vue","angular","nodejs","express","rest","api","graphql","websocket",
           "authentication","authorization","jwt","oauth","https","ssl","nginx",
           "docker","deployment","cicd","testing","unit","integration","selenium",
           "performance","optimization","caching","redis","cdn","responsive","design"} },
    };

    std::vector<ii::Document> docs;
    docs.reserve(256);

    // 每个类别生成约 51 篇文档（5×51=255，最后补 1 篇）
    ii::DocId next_id = 1;
    int cat_idx = 0;

    for (int i = 0; i < 256; ++i, ++next_id) {
        const Category& cat = categories[cat_idx % 5];
        cat_idx++;

        // 构造标题
        std::string title = cat.title_prefix + " Guide Part " + std::to_string(i + 1);

        // 构造正文：从 keyword 池中循环取词，凑够约 90 词
        std::string body;
        const auto& kw = cat.keywords;
        int body_words = 0;
        for (int w = 0; body_words < 90; ++w) {
            const std::string& word = kw[w % kw.size()];
            body += word + " ";
            ++body_words;
            // 每隔几个词插入 connective
            if (body_words % 7 == 0) body += "and ";
            if (body_words % 11 == 0) body += "for ";
            if (body_words % 13 == 0) body += "with ";
        }
        // 补充一句话
        body += "This comprehensive guide covers all essential concepts "
                "and practical examples for " + cat.name + " developers.";

        ii::Document doc;
        doc.doc_id    = next_id;
        doc.set("title", title);
        doc.set("body", body);
        doc.set("category", cat.name);
        doc.set("page_rank", 0.1f + (float)(i % 10) * 0.05f);

        docs.push_back(std::move(doc));
    }
    return docs;
}

// ── 主程序 ─────────────────────────────────────────────────────────────────────

int main() {
    const std::string INDEX_DIR = "/tmp/ii_demo_index";

    std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "   Inverted Index Demo  (256 docs × ~100 words each)  \n";
    std::cout << "═══════════════════════════════════════════════════════\n\n";

    // ── Phase 1: 写入索引 ────────────────────────────────────────────────────
    std::cout << "─── Phase 1: Indexing ──────────────────────────────────\n";
    {
        // RAM buffer 设小（0.5MB）以便触发多次 flush，演示多 Segment 效果
        ii::IndexWriter writer(INDEX_DIR, 0.5f);

        auto docs = buildDocuments();
        std::cout << "Writing " << docs.size() << " documents...\n";

        for (const auto& doc : docs) {
            writer.addDocument(doc);
        }

        writer.commit();  // flush 剩余 + 持久化

        std::cout << "\nIndexing complete.\n";
        std::cout << "  Total docs   : " << writer.totalDocs()    << "\n";
        std::cout << "  Segments     : " << writer.segmentCount() << "\n\n";
    }

    // ── Phase 2: 查询 ────────────────────────────────────────────────────────
    std::cout << "─── Phase 2: Searching ─────────────────────────────────\n";
    {
        ii::IndexSearcher searcher(INDEX_DIR);
        std::cout << "\n";

        struct QueryTest {
            std::string query;
            ii::QueryMode mode;
            int top_k;
        };

        std::vector<QueryTest> queries = {
            { "python tutorial",          ii::QueryMode::AND, 5 },
            { "java spring boot",         ii::QueryMode::AND, 5 },
            { "machine learning neural",  ii::QueryMode::AND, 5 },
            { "data science pandas",      ii::QueryMode::AND, 5 },
            { "python java",              ii::QueryMode::OR,  8 },
            { "web development api",      ii::QueryMode::AND, 5 },
            { "learning algorithm",       ii::QueryMode::OR,  5 },
        };

        for (const auto& qt : queries) {
            std::cout << "┌─────────────────────────────────────────────────\n";
            std::cout << "│ Query : \"" << qt.query << "\"\n";
            std::cout << "│ Mode  : " << (qt.mode == ii::QueryMode::AND ? "AND" : "OR")
                      << "  TopK=" << qt.top_k << "\n";
            std::cout << "├─────────────────────────────────────────────────\n";

            auto results = searcher.search(qt.query, qt.top_k, qt.mode);
            ii::IndexSearcher::printResults(results);
            std::cout << "└─────────────────────────────────────────────────\n\n";
        }
    }

    // ── Phase 3: 组件单元演示 ────────────────────────────────────────────────
    std::cout << "─── Phase 3: Component Demos ───────────────────────────\n\n";

    // 3a. Analyzer 演示
    {
        std::cout << "[Analyzer Demo]\n";
        ii::Analyzer analyzer;
        std::string text = "Python programming tutorials for beginners learning "
                           "machine learning algorithms and data science techniques";
        auto tokens = analyzer.analyze(1, text);
        std::cout << "  Input : \"" << text << "\"\n";
        std::cout << "  Tokens(" << tokens.size() << "): ";
        for (const auto& t : tokens) std::cout << t.term << " ";
        std::cout << "\n\n";
    }

    // 3b. PForDelta 演示
    {
        std::cout << "[PForDelta Demo]\n";
        std::vector<ii::DocId> doc_ids = {
            2, 4, 7, 8, 9, 12, 15, 16, 20, 25,
            28, 30, 35, 36, 38, 40, 44, 47, 50, 55,
            60, 65, 70, 75, 80, 85, 90, 95, 100, 110,
            120, 130, 140, 150, 160, 170, 180, 190, 200, 256
        };

        std::vector<ii::SkipNode> skip_nodes;
        auto compressed = ii::PForDelta::compress(doc_ids, skip_nodes);

        std::cout << "  DocIDs  : " << doc_ids.size() << " docs\n";
        std::cout << "  Original: " << doc_ids.size() * 4 << " bytes (raw int32)\n";
        std::cout << "  PFD size: " << compressed.size() << " bytes\n";
        std::cout << "  Ratio   : " << std::fixed << std::setprecision(1)
                  << (100.0 * compressed.size() / (doc_ids.size() * 4)) << "%\n";
        std::cout << "  Blocks  : " << skip_nodes.size() << "\n";

        // 解压验证
        auto decoded = ii::PForDelta::decompress(
            compressed.data(), compressed.size(),
            static_cast<uint32_t>(doc_ids.size()));
        bool ok = (decoded == doc_ids);
        std::cout << "  Verify  : " << (ok ? "PASS ✓" : "FAIL ✗") << "\n\n";
    }

    // 3c. SkipList 演示
    {
        std::cout << "[SkipList Demo]\n";
        std::vector<ii::SkipNode> nodes;
        for (int i = 0; i < 5; ++i) {
            ii::SkipNode sn;
            sn.max_doc_id  = (i + 1) * 128;
            sn.byte_offset = i * 140;
            sn.max_score   = 0.3f;
            sn.doc_count   = 128;
            nodes.push_back(sn);
        }
        ii::SkipList sl(nodes);

        ii::DocId targets[] = {1, 50, 128, 200, 300, 500, 640, 700};
        for (ii::DocId t : targets) {
            auto r = sl.find(t);
            if (r.byte_offset == UINT64_MAX) {
                std::cout << "  find(" << std::setw(3) << t
                          << ") → not found (out of range)\n";
            } else {
                std::cout << "  find(" << std::setw(3) << t
                          << ") → block[" << r.block_index
                          << "]  offset=" << r.byte_offset << "\n";
            }
        }
        std::cout << "\n";
    }


    // ── Phase 4: Segment 合并与软删除 ─────────────────────────────────────────
    std::cout << "─── Phase 4: Merge & Soft Delete ───────────────────────\n\n";
    {
        ii::SegmentMerger merger(INDEX_DIR);

        std::cout << "[Before merge]\n";
        merger.printStats();

        // 软删除若干文档（模拟更新）
        std::cout << "\n[SoftDelete] Deleting DocID 1, 2, 3 (simulating updates)...\n";
        merger.softDeleteBatch({1, 2, 3});

        std::cout << "\n[After soft delete, before merge]\n";
        merger.printStats();

        // 强制合并所有 Segment → 真正清除已删除文档
        auto stats = merger.mergeAll(99);

        std::cout << "[Merge Stats]\n";
        std::cout << "  Input  segments : " << stats.input_segment_count << "\n";
        std::cout << "  Input  docs     : " << stats.input_doc_count << "\n";
        std::cout << "  Deleted docs    : " << stats.deleted_doc_count << " (physically removed)\n";
        std::cout << "  Output docs     : " << stats.output_doc_count << "\n";
        std::cout << "  Output segment  : " << stats.output_segment_id << "\n";
        std::cout << "  Output size     : " << stats.output_bytes / 1024 << " KB\n\n";

        // 合并后重新搜索，验证软删除文档不再出现
        std::cout << "[Post-merge search] Query: \"python tutorial\" (AND, Top5)\n";
        ii::IndexSearcher searcher2(INDEX_DIR);
        auto results = searcher2.search("python tutorial", 5, ii::QueryMode::AND);
        ii::IndexSearcher::printResults(results);

        // 验证被删除的 DocID 不在结果中
        bool deleted_found = false;
        for (const auto& r : results) {
            if (r.doc_id == 1 || r.doc_id == 2 || r.doc_id == 3) {
                deleted_found = true;
            }
        }
        std::cout << "\n  Deleted docs in results: " << (deleted_found ? "YES (BUG)" : "NO (correct)") << "\n";
    }

        std::cout << "═══════════════════════════════════════════════════════\n";
    std::cout << "Demo complete. Index files at: " << INDEX_DIR << "\n";
    std::cout << "═══════════════════════════════════════════════════════\n";
    return 0;
}