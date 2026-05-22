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

std::vector<uint32_t> listSegmentIds(const std::string& dir) {
    std::vector<uint32_t> ids;
    if (!fs::exists(dir)) return ids;

    for (const auto& entry : fs::directory_iterator(dir)) {
        const auto name = entry.path().filename().string();
        // 格式：_<N>.si
        if (name.size() > 4 && name.front() == '_' &&
            name.substr(name.size() - 3) == ".si") {
            try {
                uint32_t id = static_cast<uint32_t>(
                    std::stoul(name.substr(1, name.size() - 4)));
                ids.push_back(id);
            } catch (...) {}
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void deleteSegmentFiles(const std::string& dir, uint32_t seg_id) {
    const std::string prefix = "_" + std::to_string(seg_id) + ".";
    if (!fs::exists(dir)) return;

    for (const auto& entry : fs::directory_iterator(dir)) {
        if (entry.path().filename().string().rfind(prefix, 0) == 0) {
            std::error_code ec;
            fs::remove(entry.path(), ec);
            // 忽略删除失败（文件可能已不存在）
        }
    }
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
