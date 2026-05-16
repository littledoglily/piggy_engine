#pragma once
#include "types.h"
#include <string>
#include <vector>

namespace ii {

// FastFieldWriter：在 flush 时将数值字段列存写入磁盘
//
// 文件格式（_N.ff_pubtime / _N.ff_uid / _N.ff_pagerank）：
//   定长数组，下标 i 对应 local_doc_idx i（0-indexed）
//   pubtime / uid：int64_t × doc_count（每条 8 字节）
//   page_rank    ：float   × doc_count（每条 4 字节）
//
// 设计原则：
//   - 定长保证 O(1) 随机访问：offset = idx × sizeof(T)
//   - Phase 1 不压缩，Phase 2（BKD）引入块压缩
class FastFieldWriter {
public:
    FastFieldWriter() = default;

    // 追加一条文档的数值字段（与 StoredDoc 并行调用，顺序必须一致）
    void add(const FastFieldDoc& doc);

    // 将缓冲数据写入磁盘，文件名：dir/_seg_id.ff_<field>
    void flush(const std::string& dir, uint32_t seg_id) const;

    // flush 后清空缓冲，供 IndexWriter 复用
    void clear();

    size_t size() const { return pubtimes_.size(); }

private:
    std::string ffPath(const std::string& dir,
                       uint32_t seg_id,
                       const std::string& field) const;

    std::vector<int64_t> pubtimes_;
    std::vector<int64_t> uids_;
    std::vector<float>   page_ranks_;
};

} // namespace ii
