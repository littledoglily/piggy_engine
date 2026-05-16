#pragma once
#include "types.h"
#include <map>
#include <string>
#include <vector>
#include <cstdint>

namespace ii {

// ── FastField 写入统计（flush 时采集）────────────────────────────────────────
struct FFWriteStats {
    std::map<std::string, uint64_t> file_bytes;  // 字段名 → 文件字节数
    uint64_t total_bytes = 0;
    uint64_t total_us    = 0;                    // 写入耗时（微秒）
};

// FastFieldWriter：将数值字段按列写入磁盘
//
// 文件格式：_N.ff_<field_name>
//   int64 字段：int64_t × doc_count（8 字节/条）
//   float 字段：float   × doc_count（4 字节/条）
//
// 向后兼容：保留 add(FastFieldDoc) 方法，内部路由到 addInt64/addFloat32。
class FastFieldWriter {
public:
    FastFieldWriter() = default;

    // ── 泛化接口（Schema 驱动）────────────────────────────────────────────────
    void addInt64  (const std::string& field, int64_t val);
    void addFloat32(const std::string& field, float   val);

    FFWriteStats flush(const std::string& dir, uint32_t seg_id) const;
    void clear();

    size_t size() const;

    // ── 向后兼容（旧 FastFieldDoc 路由）────────────────────────────────────
    void add(const FastFieldDoc& doc);

private:
    std::string ffPath(const std::string& dir,
                       uint32_t seg_id,
                       const std::string& field) const;

    std::map<std::string, std::vector<int64_t>> int64_fields_;
    std::map<std::string, std::vector<float>>   float32_fields_;
};

} // namespace ii
