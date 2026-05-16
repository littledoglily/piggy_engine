#pragma once
#include "types.h"
#include "schema/schema.h"
#include <map>
#include <string>
#include <vector>

namespace ii {

// FastFieldReader：从磁盘加载数值列存，提供 O(1) 随机访问 + 范围过滤
//
// Phase 1：启动时全列加载到内存（简单、直接）
// Phase 2（未来）：改为 BKD tree 驱动，列文件只作 fallback
//
// 向后兼容：保留旧的命名访问方法（pubtime/uid/pageRank）和旧构造函数。
class FastFieldReader {
public:
    // ── 新接口（Schema 驱动）──────────────────────────────────────────────────
    FastFieldReader(const std::string& dir, uint32_t seg_id,
                    uint32_t doc_count, const Schema& schema);

    // ── 向后兼容（使用 defaultSchema()）────────────────────────────────────
    FastFieldReader(const std::string& dir, uint32_t seg_id, uint32_t doc_count);

    // ── 泛化访问接口 ─────────────────────────────────────────────────────────
    bool    hasField  (const std::string& field) const;
    int64_t getInt64  (const std::string& field, uint32_t idx) const;
    float   getFloat32(const std::string& field, uint32_t idx) const;

    std::vector<uint32_t> filterInt64(const std::string& field,
                                      int64_t lo, int64_t hi) const;

    // ── 向后兼容命名访问 ─────────────────────────────────────────────────────
    int64_t pubtime  (uint32_t idx) const { return getInt64  ("pubtime",   idx); }
    int64_t uid      (uint32_t idx) const { return getInt64  ("uid",       idx); }
    float   pageRank (uint32_t idx) const { return getFloat32("page_rank", idx); }

    std::vector<uint32_t> filterPubtime(int64_t lo, int64_t hi) const {
        return filterInt64("pubtime", lo, hi);
    }
    std::vector<uint32_t> filterUid(int64_t uid_val) const {
        return filterInt64("uid", uid_val, uid_val);
    }

    // ── 整列读取（排序 / 聚合用，向后兼容）──────────────────────────────────
    const std::vector<int64_t>& allPubtimes()  const;
    const std::vector<int64_t>& allUids()      const;
    const std::vector<float>&   allPageRanks() const;

    uint32_t docCount() const { return doc_count_; }
    bool     hasData()  const { return !int64_fields_.empty() || !float32_fields_.empty(); }

private:
    void load(const std::string& dir, uint32_t seg_id, const Schema& schema);

    std::string ffPath(const std::string& dir,
                       uint32_t seg_id,
                       const std::string& field) const;

    static const std::vector<int64_t>& emptyI64();
    static const std::vector<float>&   emptyF32();

    uint32_t doc_count_;
    std::map<std::string, std::vector<int64_t>> int64_fields_;
    std::map<std::string, std::vector<float>>   float32_fields_;
};

} // namespace ii
