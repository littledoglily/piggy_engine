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

// 扫描目录，返回所有 _N.si 文件对应的 segment id，升序
std::vector<uint32_t> listSegmentIds(const std::string& dir);

// 删除指定 segment 的全部关联文件（_N.si / .tim_* / .doc_* / .pos_*
// / .len_* / .fdt / .fdx / .liv / .ff_*）
void deleteSegmentFiles(const std::string& dir, uint32_t seg_id);

// 检查文件是否存在
bool fileExists(const std::string& path);

// 返回文件大小（字节），不存在时返回 0
uint64_t fileSize(const std::string& path);

} // namespace ii::file_utils
