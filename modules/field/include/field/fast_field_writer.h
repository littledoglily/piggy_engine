#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// field/fast_field_writer.h  —  数值列存写入
// ─────────────────────────────────────────────────────────────────────────────
#include "core/types.h"
#include <map>
#include <string>
#include <vector>
#include <cstdint>

namespace ii {

struct FFWriteStats {
    std::map<std::string, uint64_t> file_bytes;
    uint64_t total_bytes = 0;
    uint64_t total_us    = 0;
};

class FastFieldWriter {
public:
    FastFieldWriter() = default;

    void addInt64  (const std::string& field, int64_t val);
    void addFloat32(const std::string& field, float   val);

    FFWriteStats flush(const std::string& dir, uint32_t seg_id) const;
    void   clear();
    size_t size() const;

    void add(const FastFieldDoc& doc);  // 向后兼容

private:
    std::string ffPath(const std::string& dir, uint32_t seg_id,
                       const std::string& field) const;

    std::map<std::string, std::vector<int64_t>> int64_fields_;
    std::map<std::string, std::vector<float>>   float32_fields_;
};

} // namespace ii
