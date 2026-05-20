#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// field/fast_field_reader.h  —  数值列存读取
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include "field/schema.h"
#include <map>
#include <string>
#include <vector>

namespace ii {

class FastFieldReader {
public:
    FastFieldReader(const std::string& dir, uint32_t seg_id,
                    uint32_t doc_count, const Schema& schema);
    FastFieldReader(const std::string& dir, uint32_t seg_id, uint32_t doc_count);

    bool    hasField  (const std::string& field) const;
    int64_t getInt64  (const std::string& field, uint32_t idx) const;
    float   getFloat32(const std::string& field, uint32_t idx) const;

    std::vector<uint32_t> filterInt64(const std::string& field,
                                      int64_t lo, int64_t hi) const;

    int64_t pubtime  (uint32_t idx) const { return getInt64  ("pubtime",   idx); }
    int64_t uid      (uint32_t idx) const { return getInt64  ("uid",       idx); }
    float   pageRank (uint32_t idx) const { return getFloat32("page_rank", idx); }

    std::vector<uint32_t> filterPubtime(int64_t lo, int64_t hi) const {
        return filterInt64("pubtime", lo, hi);
    }
    std::vector<uint32_t> filterUid(int64_t uid_val) const {
        return filterInt64("uid", uid_val, uid_val);
    }

    const std::vector<int64_t>& allPubtimes()  const;
    const std::vector<int64_t>& allUids()      const;
    const std::vector<float>&   allPageRanks() const;

    uint32_t docCount() const { return doc_count_; }
    bool     hasData()  const { return !int64_fields_.empty() || !float32_fields_.empty(); }

private:
    void load(const std::string& dir, uint32_t seg_id, const Schema& schema);
    std::string ffPath(const std::string& dir, uint32_t seg_id,
                       const std::string& field) const;
    static const std::vector<int64_t>& emptyI64();
    static const std::vector<float>&   emptyF32();

    uint32_t doc_count_;
    std::map<std::string, std::vector<int64_t>> int64_fields_;
    std::map<std::string, std::vector<float>>   float32_fields_;
};

} // namespace ii
