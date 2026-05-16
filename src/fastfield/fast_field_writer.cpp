#include "fastfield/fast_field_writer.h"
#include <fstream>
#include <stdexcept>

namespace ii {

std::string FastFieldWriter::ffPath(const std::string& dir,
                                     uint32_t seg_id,
                                     const std::string& field) const {
    return dir + "/_" + std::to_string(seg_id) + ".ff_" + field;
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

void FastFieldWriter::flush(const std::string& dir, uint32_t seg_id) const {
    for (const auto& [field, data] : int64_fields_) {
        std::ofstream f(ffPath(dir, seg_id, field),
                        std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot open ff file: " + field);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size() * sizeof(int64_t)));
    }
    for (const auto& [field, data] : float32_fields_) {
        std::ofstream f(ffPath(dir, seg_id, field),
                        std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot open ff file: " + field);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size() * sizeof(float)));
    }
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
