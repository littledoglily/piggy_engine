#include "fastfield/fast_field_reader.h"
#include <fstream>
#include <stdexcept>

namespace ii {

std::string FastFieldReader::ffPath(const std::string& dir,
                                     uint32_t seg_id,
                                     const std::string& field) const {
    return dir + "/_" + std::to_string(seg_id) + ".ff_" + field;
}

FastFieldReader::FastFieldReader(const std::string& dir, uint32_t seg_id,
                                 uint32_t doc_count, const Schema& schema)
    : doc_count_(doc_count)
{
    load(dir, seg_id, schema);
}

FastFieldReader::FastFieldReader(const std::string& dir, uint32_t seg_id,
                                 uint32_t doc_count)
    : doc_count_(doc_count)
{
    load(dir, seg_id, Schema::defaultSchema());
}

void FastFieldReader::load(const std::string& dir, uint32_t seg_id,
                            const Schema& schema) {
    for (const auto* fs : schema.fastFields()) {
        if (fs->type == FieldType::Int64) {
            std::ifstream f(ffPath(dir, seg_id, fs->name), std::ios::binary);
            if (!f) continue;
            auto& vec = int64_fields_[fs->name];
            vec.resize(doc_count_);
            f.read(reinterpret_cast<char*>(vec.data()),
                   static_cast<std::streamsize>(doc_count_ * sizeof(int64_t)));
        } else if (fs->type == FieldType::Float32) {
            std::ifstream f(ffPath(dir, seg_id, fs->name), std::ios::binary);
            if (!f) continue;
            auto& vec = float32_fields_[fs->name];
            vec.resize(doc_count_);
            f.read(reinterpret_cast<char*>(vec.data()),
                   static_cast<std::streamsize>(doc_count_ * sizeof(float)));
        }
    }
}

// ── 泛化访问 ──────────────────────────────────────────────────────────────────

bool FastFieldReader::hasField(const std::string& field) const {
    return int64_fields_.count(field) || float32_fields_.count(field);
}

int64_t FastFieldReader::getInt64(const std::string& field, uint32_t idx) const {
    auto it = int64_fields_.find(field);
    if (it == int64_fields_.end()) return 0;
    return (idx < it->second.size()) ? it->second[idx] : 0;
}

float FastFieldReader::getFloat32(const std::string& field, uint32_t idx) const {
    auto it = float32_fields_.find(field);
    if (it == float32_fields_.end()) return 0.0f;
    return (idx < it->second.size()) ? it->second[idx] : 0.0f;
}

std::vector<uint32_t> FastFieldReader::filterInt64(const std::string& field,
                                                    int64_t lo, int64_t hi) const {
    std::vector<uint32_t> result;
    auto it = int64_fields_.find(field);
    if (it == int64_fields_.end()) return result;
    const auto& vec = it->second;
    for (uint32_t i = 0; i < static_cast<uint32_t>(vec.size()); ++i) {
        if (vec[i] >= lo && vec[i] <= hi) result.push_back(i);
    }
    return result;
}

// ── 整列读取（向后兼容）──────────────────────────────────────────────────────

const std::vector<int64_t>& FastFieldReader::emptyI64() {
    static const std::vector<int64_t> e;
    return e;
}
const std::vector<float>& FastFieldReader::emptyF32() {
    static const std::vector<float> e;
    return e;
}

const std::vector<int64_t>& FastFieldReader::allPubtimes() const {
    auto it = int64_fields_.find("pubtime");
    return (it != int64_fields_.end()) ? it->second : emptyI64();
}
const std::vector<int64_t>& FastFieldReader::allUids() const {
    auto it = int64_fields_.find("uid");
    return (it != int64_fields_.end()) ? it->second : emptyI64();
}
const std::vector<float>& FastFieldReader::allPageRanks() const {
    auto it = float32_fields_.find("page_rank");
    return (it != float32_fields_.end()) ? it->second : emptyF32();
}

} // namespace ii
