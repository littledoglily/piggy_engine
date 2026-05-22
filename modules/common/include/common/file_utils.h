#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// common/file_utils.h  —  Segment 文件操作工具
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <string>
#include <vector>

namespace ii::file_utils {

// 确保目录存在（递归创建）
void ensureDir(const std::string& dir);

// 返回 segment 子目录路径：<dir>/segment_<seg_id>
std::string segmentDir(const std::string& dir, uint32_t seg_id);

// 扫描目录，返回所有含 .done 标记的 segment_N/ 子目录对应的 segment id，升序
std::vector<uint32_t> listSegmentIds(const std::string& dir);

// 删除指定 segment 的整个子目录（segment_N/）
void deleteSegmentFiles(const std::string& dir, uint32_t seg_id);

// 检查文件是否存在
bool fileExists(const std::string& path);

// 返回文件大小（字节），不存在时返回 0
uint64_t fileSize(const std::string& path);

} // namespace ii::file_utils
