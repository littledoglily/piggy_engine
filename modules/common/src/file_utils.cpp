#include "common/file_utils.h"
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace ii::file_utils {

void ensureDir(const std::string& dir) {
    fs::create_directories(dir);
}

std::string segmentDir(const std::string& dir, uint32_t seg_id) {
    return dir + "/segment_" + std::to_string(seg_id);
}

std::vector<uint32_t> listSegmentIds(const std::string& dir) {
    std::vector<uint32_t> ids;
    if (!fs::exists(dir)) return ids;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_directory()) continue;
        const auto name = entry.path().filename().string();
        // 格式：segment_<N>/，且含 .done 标记（写完成）
        if (name.size() > 8 && name.substr(0, 8) == "segment_") {
            try {
                uint32_t id = static_cast<uint32_t>(std::stoul(name.substr(8)));
                if (fs::exists(entry.path() / ".done"))
                    ids.push_back(id);
            } catch (...) {}
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void deleteSegmentFiles(const std::string& dir, uint32_t seg_id) {
    fs::path seg_dir = segmentDir(dir, seg_id);
    std::error_code ec;
    fs::remove_all(seg_dir, ec);
}

bool fileExists(const std::string& path) {
    return fs::exists(path);
}

uint64_t fileSize(const std::string& path) {
    std::error_code ec;
    auto sz = fs::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(sz);
}

} // namespace ii::file_utils
