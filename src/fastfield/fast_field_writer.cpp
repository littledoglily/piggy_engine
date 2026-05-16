#include "fastfield/fast_field_writer.h"
#include <fstream>
#include <stdexcept>

namespace ii {

std::string FastFieldWriter::ffPath(const std::string& dir,
                                     uint32_t seg_id,
                                     const std::string& field) const {
    return dir + "/_" + std::to_string(seg_id) + ".ff_" + field;
}

void FastFieldWriter::add(const FastFieldDoc& doc) {
    pubtimes_.push_back(doc.pubtime);
    uids_.push_back(doc.uid);
    page_ranks_.push_back(doc.page_rank);
}

void FastFieldWriter::flush(const std::string& dir, uint32_t seg_id) const {
    auto write_i64 = [&](const std::string& field,
                          const std::vector<int64_t>& data) {
        std::ofstream f(ffPath(dir, seg_id, field),
                        std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot open ff file: " + field);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size() * sizeof(int64_t)));
    };
    auto write_f32 = [&](const std::string& field,
                          const std::vector<float>& data) {
        std::ofstream f(ffPath(dir, seg_id, field),
                        std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("Cannot open ff file: " + field);
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size() * sizeof(float)));
    };

    write_i64("pubtime",   pubtimes_);
    write_i64("uid",       uids_);
    write_f32("page_rank", page_ranks_);
}

void FastFieldWriter::clear() {
    pubtimes_.clear();
    uids_.clear();
    page_ranks_.clear();
}

} // namespace ii
