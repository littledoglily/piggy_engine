#pragma once
#include "types.h"
#include <string>
#include <vector>

namespace ii {

// FastFieldReader：从磁盘加载数值列存，提供 O(1) 随机访问 + 范围过滤
//
// Phase 1：启动时全列加载到内存（简单、直接）
//   - 百万文档时 pubtime 列约 8MB，uid 约 8MB，page_rank 约 4MB，可接受
// Phase 2：改为 BKD tree 驱动，列文件只作 fallback
//
// 下标约定：local_doc_idx 均为 0-indexed（segment 内第 0 篇文档）
class FastFieldReader {
public:
    // 构造时加载所有列；若文件不存在则静默跳过（兼容旧 Segment）
    FastFieldReader(const std::string& dir, uint32_t seg_id, uint32_t doc_count);

    // ── 单值读取（O(1)）──────────────────────────────────────────────────────
    int64_t pubtime(uint32_t local_doc_idx)   const;
    int64_t uid(uint32_t local_doc_idx)       const;
    float   pageRank(uint32_t local_doc_idx)  const;

    // ── 范围过滤（O(N) 全列扫描，Phase 2 改为 BKD）────────────────────────
    // 返回满足条件的 local_doc_idx 列表（0-indexed，已排序）
    std::vector<uint32_t> filterPubtime(int64_t lo, int64_t hi) const;
    std::vector<uint32_t> filterUid(int64_t uid_val)            const;

    // ── 整列读取（排序 / 聚合用）──────────────────────────────────────────
    const std::vector<int64_t>& allPubtimes()  const { return pubtimes_; }
    const std::vector<int64_t>& allUids()      const { return uids_; }
    const std::vector<float>&   allPageRanks() const { return page_ranks_; }

    uint32_t docCount()   const { return doc_count_; }
    bool     hasData()    const { return !pubtimes_.empty(); }

private:
    void load(const std::string& dir, uint32_t seg_id);

    std::string ffPath(const std::string& dir,
                       uint32_t seg_id,
                       const std::string& field) const;

    uint32_t             doc_count_;
    std::vector<int64_t> pubtimes_;
    std::vector<int64_t> uids_;
    std::vector<float>   page_ranks_;
};

} // namespace ii
