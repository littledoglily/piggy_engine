#include "fastfield/fast_field_reader.h"
#include <fstream>
#include <stdexcept>

namespace ii {

std::string FastFieldReader::ffPath(const std::string& dir,
                                     uint32_t seg_id,
                                     const std::string& field) const {
    return dir + "/_" + std::to_string(seg_id) + ".ff_" + field;
}

FastFieldReader::FastFieldReader(const std::string& dir,
                                 uint32_t seg_id,
                                 uint32_t doc_count)
    : doc_count_(doc_count)
{
    load(dir, seg_id);
}

void FastFieldReader::load(const std::string& dir, uint32_t seg_id) {
    auto read_i64 = [&](const std::string& field,
                         std::vector<int64_t>& out) {
        std::ifstream f(ffPath(dir, seg_id, field), std::ios::binary);
        if (!f) return;  // 旧 Segment 无 FF 文件，静默跳过
        out.resize(doc_count_);
        f.read(reinterpret_cast<char*>(out.data()),
               static_cast<std::streamsize>(doc_count_ * sizeof(int64_t)));
    };
    auto read_f32 = [&](const std::string& field,
                         std::vector<float>& out) {
        std::ifstream f(ffPath(dir, seg_id, field), std::ios::binary);
        if (!f) return;
        out.resize(doc_count_);
        f.read(reinterpret_cast<char*>(out.data()),
               static_cast<std::streamsize>(doc_count_ * sizeof(float)));
    };

    read_i64("pubtime",   pubtimes_);
    read_i64("uid",       uids_);
    read_f32("page_rank", page_ranks_);
}

// ── 单值读取（O(1)）──────────────────────────────────────────────────────────

int64_t FastFieldReader::pubtime(uint32_t idx) const {
    return (idx < pubtimes_.size()) ? pubtimes_[idx] : 0;
}

int64_t FastFieldReader::uid(uint32_t idx) const {
    return (idx < uids_.size()) ? uids_[idx] : 0;
}

float FastFieldReader::pageRank(uint32_t idx) const {
    return (idx < page_ranks_.size()) ? page_ranks_[idx] : 0.0f;
}

// ── 范围过滤（O(N) 全列扫描）────────────────────────────────────────────────

std::vector<uint32_t> FastFieldReader::filterPubtime(int64_t lo, int64_t hi) const {
    std::vector<uint32_t> result;
    for (uint32_t i = 0; i < static_cast<uint32_t>(pubtimes_.size()); ++i) {
        if (pubtimes_[i] >= lo && pubtimes_[i] <= hi)
            result.push_back(i);
    }
    return result;  // already ascending
}

std::vector<uint32_t> FastFieldReader::filterUid(int64_t uid_val) const {
    std::vector<uint32_t> result;
    for (uint32_t i = 0; i < static_cast<uint32_t>(uids_.size()); ++i) {
        if (uids_[i] == uid_val)
            result.push_back(i);
    }
    return result;
}

} // namespace ii
