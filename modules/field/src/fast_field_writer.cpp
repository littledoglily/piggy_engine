#include "field/fast_field_writer.h"
#include <fstream>
#include <filesystem>
#include <chrono>
#include <stdexcept>

namespace ii {

std::string FastFieldWriter::ffPath(const std::string& dir,
                                     uint32_t seg_id,
                                     const std::string& field) const {
    return dir + "/segment_" + std::to_string(seg_id) + "/ff_" + field;
}

void FastFieldWriter::addInt64(const std::string& field, int64_t val) {
    int64_fields_[field].push_back(val);
}

void FastFieldWriter::addFloat32(const std::string& field, float val) {
    float32_fields_[field].push_back(val);
}

// 向后兼容：FastFieldDoc 路由到具体字段
void FastFieldWriter::add(const FastFieldDoc& doc) {
    addInt64  ("pubtime",   doc.pubtime);
    addInt64  ("uid",       doc.uid);
    addFloat32("page_rank", doc.page_rank);
}

FFWriteStats FastFieldWriter::flush(const std::string& dir, uint32_t seg_id) const {
    using Clock = std::chrono::steady_clock;
    FFWriteStats stats;
    auto t0 = Clock::now();

    // 确保 segment 子目录存在（FastFieldWriter 可能在 SegmentWriter 之前被独立调用）
    std::filesystem::create_directories(
        dir + "/segment_" + std::to_string(seg_id));

    for (const auto& [field, data] : int64_fields_) {
        auto p = ffPath(dir, seg_id, field);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot open ff file: " + field);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size() * sizeof(int64_t)));
        f.close();
        uint64_t bytes = std::filesystem::file_size(p);
        stats.file_bytes[field] = bytes;
        stats.total_bytes += bytes;
    }
    for (const auto& [field, data] : float32_fields_) {
        auto p = ffPath(dir, seg_id, field);
        std::ofstream f(p, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot open ff file: " + field);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size() * sizeof(float)));
        f.close();
        uint64_t bytes = std::filesystem::file_size(p);
        stats.file_bytes[field] = bytes;
        stats.total_bytes += bytes;
    }

    stats.total_us = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - t0).count();
    return stats;
}

void FastFieldWriter::clear() {
    int64_fields_.clear();
    float32_fields_.clear();
}

size_t FastFieldWriter::size() const {
    if (!int64_fields_.empty())   return int64_fields_.begin()->second.size();
    if (!float32_fields_.empty()) return float32_fields_.begin()->second.size();
    return 0;
}

} // namespace ii
