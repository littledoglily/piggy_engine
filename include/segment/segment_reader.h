#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// segment_reader.h  —  只读方式打开并查询一个 Segment
//
// 模拟 Lucene 的 mmap 懒加载：
//   .tim  → 启动时全部读入 std::map（模拟 JVM 堆常驻）
//   .doc  → 按需 seek + read（模拟 mmap 缺页加载）
//   .fdt  → 命中后按偏移读取（模拟按需加载）
//   .liv  → 启动时读入 bitset（模拟常驻内存）
// ─────────────────────────────────────────────────────────────────────────────
#include "types.h"
#include "postings/skiplist.h"
#include "fastfield/fast_field_reader.h"
#include <string>
#include <map>
#include <unordered_map>
#include <vector>
#include <fstream>
#include <bitset>
#include <memory>

namespace ii {

class SegmentReader {
public:
    static constexpr uint32_t MAX_DOCS = 65536;  // 示例上限

    explicit SegmentReader(const std::string& dir, uint32_t segment_id);

    // 检查文档是否存活（未被软删除）
    bool isAlive(DocId doc_id) const;

    // 软删除一个文档（修改 .liv 位图）
    void softDelete(DocId doc_id);

    // 查找 term 的元数据（未找到返回 nullptr）
    const TermMeta* getTermMeta(const std::string& term) const;

    // 解压某 term 的完整 posting list（doc_id 列表）
    // 内部使用 SkipList 定位 Block，再 PForDelta 解压
    std::vector<DocId> readPostingList(const std::string& term) const;

    // 读取某 term 的 posting list 中满足 >= target_doc_id 的部分
    // 用于 Zigzag AND 中的快速跳跃
    std::vector<DocId> readPostingListFrom(const std::string& term,
                                           DocId target_doc_id) const;

    // 从 .pos 读取某 term 所有文档的 <doc_id, tf, positions>
    std::vector<PostingEntry> readPosEntries(const std::string& term) const;

    // 从 .fdt 按 doc_id 读取原文（返回 StoredDoc）
    struct StoredDocResult {
        DocId       doc_id = 0;
        uint64_t    ext_id = 0;
        std::string source;
        std::string title;
        std::string body;
        std::string category;
    };
    StoredDocResult readStoredDoc(DocId doc_id) const;

    uint32_t docCount()  const { return doc_count_; }
    uint32_t termCount() const { return term_dict_.size(); }
    uint32_t segmentId() const { return seg_id_; }

    // 暴露词典（工具/调试用）
    const std::map<std::string, TermMeta>& termDict() const { return term_dict_; }

    // 计算 BM25 score（单 doc，多 term）
    // term_idfs：由 IndexSearcher 基于全局统计预算的 {term -> global_idf}
    float bm25Score(DocId doc_id,
                    const std::vector<std::string>& query_terms,
                    const std::unordered_map<std::string, float>& term_idfs) const;

    // ── FastField 接口（数值列存）─────────────────────────────────────────────
    // 泛化访问：通过 FastFieldReader 泛化接口
    const FastFieldReader& ff() const { return *ff_; }
    bool hasFastField() const;

    // 向后兼容命名访问（local_doc_idx 为 0-indexed）
    int64_t ffPubtime(uint32_t local_doc_idx)  const;
    int64_t ffUid(uint32_t local_doc_idx)      const;
    float   ffPageRank(uint32_t local_doc_idx) const;

    // 范围过滤：返回满足条件的 local_doc_idx（0-indexed）
    std::vector<uint32_t> filterPubtime(int64_t lo, int64_t hi) const;
    std::vector<uint32_t> filterUid(int64_t uid_val)            const;

private:
    // 加载 .tim 词典到内存
    void loadTim();

    // 加载 .fdx 偏移表
    void loadFdx();

    // 加载 .liv 位图
    void loadLiv();

    // 读取 term 对应的 SkipList（懒加载，首次访问时 seek 到 .doc）
    SkipList readSkipList(const TermMeta& meta) const;

    // 文件路径辅助
    std::string path(const std::string& ext) const;

    std::string dir_;
    uint32_t    seg_id_;
    uint32_t    doc_count_ = 0;
    float       avg_doc_len_ = 0.0f;

    // 常驻内存（模拟 JVM 堆）
    std::map<std::string, TermMeta> term_dict_;   // .tim 全量加载
    std::vector<uint64_t>           fdx_offsets_; // .fdx 全量加载
    std::vector<bool>               liv_bitmap_;  // .liv 全量加载

    // 磁盘文件（按需 seek 读取）
    mutable std::ifstream doc_file_;   // .doc
    mutable std::ifstream pos_file_;   // .pos
    mutable std::ifstream fdt_file_;   // .fdt

    // 数值列存（构造时加载）
    std::unique_ptr<FastFieldReader> ff_;
};

} // namespace ii
