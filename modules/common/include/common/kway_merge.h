#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// common/kway_merge.h  —  泛型 K-way 最小堆合并迭代器
//
// 用法：
//   KwayMerge<int> merger;
//   merger.addSource([&]() -> std::optional<int> { ... }); // 每个已排序序列
//   merger.init();
//   int val; size_t src;
//   while (merger.next(val, &src)) { ... }
//
// 要求：
//   - 每个 source 函数调用一次返回下一个元素，耗尽时返回 std::nullopt
//   - 各 source 序列内部必须已按 Compare 排好序（默认升序）
//   - Compare 与 std::priority_queue 的 Compare 语义一致（less = 小的先出）
// ─────────────────────────────────────────────────────────────────────────────
#include <cassert>
#include <functional>
#include <optional>
#include <queue>
#include <vector>

namespace ii {

template<typename T, typename Compare = std::less<T>>
class KwayMerge {
public:
    using Source = std::function<std::optional<T>()>;

    void addSource(Source src) {
        sources_.push_back(std::move(src));
    }

    // 拉取所有 source 的首个元素，建立初始堆。
    // 必须在所有 addSource() 之后、next() 之前调用一次。
    void init() {
        assert(heap_.empty() && "init() called twice");
        for (size_t i = 0; i < sources_.size(); ++i) {
            auto val = sources_[i]();
            if (val) heap_.push({std::move(*val), i});
        }
    }

    // 取出当前最小元素写入 out，source 下标写入 src_idx（可为 nullptr）。
    // 所有 source 耗尽后返回 false。
    bool next(T& out, size_t* src_idx = nullptr) {
        if (heap_.empty()) return false;

        auto top = std::move(const_cast<Entry&>(heap_.top()));
        heap_.pop();

        out = std::move(top.value);
        if (src_idx) *src_idx = top.idx;

        // 从同一 source 拉取下一个元素补充堆
        auto nxt = sources_[top.idx]();
        if (nxt) heap_.push({std::move(*nxt), top.idx});

        return true;
    }

    bool empty() const { return heap_.empty(); }
    size_t sourceCount() const { return sources_.size(); }

private:
    struct Entry {
        T      value;
        size_t idx;

        // 最小堆：Compare(a, b) 为 true 表示 a 优先（a < b 则 a 先出）
        // priority_queue 默认最大堆，需要反转比较
        bool operator>(const Entry& o) const {
            return Compare{}(o.value, value); // o < this → this 劣先
        }
    };

    // 使用 greater 使 priority_queue 成为最小堆
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap_;
    std::vector<Source> sources_;
};

} // namespace ii
