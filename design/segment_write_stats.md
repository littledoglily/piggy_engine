# Segment 构建统计信息设计方案

## 目标

每次 segment flush 完成后，打印一次完整的构建统计，包含：
- 每个 term 的 posting list entry 数（df）和 skipnode 数的聚合统计
- 每个文件的字节大小（.tim / .doc / .pos / .fdt / .fdx / .liv / .si / .ff_*）
- 写各文件消耗的时间（微秒精度）
- 全局汇总（总大小、总写入时间）

---

## 数据结构

### SegmentWriteStats（新增到 segment_writer.h）

```cpp
struct SegmentWriteStats {
    // ── 基本信息 ──────────────────────────────────────────────────────────────
    uint32_t    segment_id;
    uint32_t    doc_count;
    uint32_t    term_count;

    // ── Posting List 聚合统计 ─────────────────────────────────────────────────
    // df（每个 term 出现的文档数）聚合
    uint64_t    total_pl_entries;   // Σ df（所有 term 的 df 之和）
    uint32_t    max_pl_df;          // 最大 df（最高频 term）
    std::string max_pl_term;        // df 最大的 term
    float       avg_pl_df;          // 平均 df = total_pl_entries / term_count

    // SkipNode 聚合（每个 term 的 skipnode 数 = ceil(df / 128)）
    uint64_t    total_skip_nodes;           // 所有 term skipnode 总数
    uint32_t    max_skip_nodes;             // 单个 term 最多 skipnode 数
    std::string max_skip_term;              // skipnode 最多的 term
    float       avg_skip_nodes_per_term;    // 平均 skipnode 数 / term

    // ── 文件大小（字节）──────────────────────────────────────────────────────
    uint64_t    tim_bytes   = 0;
    uint64_t    doc_bytes   = 0;
    uint64_t    pos_bytes   = 0;
    uint64_t    fdt_bytes   = 0;
    uint64_t    fdx_bytes   = 0;
    uint64_t    liv_bytes   = 0;
    uint64_t    si_bytes    = 0;

    // ── 写文件耗时（微秒）────────────────────────────────────────────────────
    uint64_t    tim_us      = 0;
    uint64_t    doc_us      = 0;
    uint64_t    pos_us      = 0;
    uint64_t    fdt_fdx_us  = 0;

    // ── 辅助计算 ─────────────────────────────────────────────────────────────
    uint64_t totalSegBytes() const {
        return tim_bytes + doc_bytes + pos_bytes +
               fdt_bytes + fdx_bytes + liv_bytes + si_bytes;
    }
    uint64_t totalSegUs() const {
        return tim_us + doc_us + pos_us + fdt_fdx_us;
    }
};
```

### FFWriteStats（新增到 fast_field_writer.h）

```cpp
struct FFWriteStats {
    std::map<std::string, uint64_t> file_bytes;  // 字段名 → 文件字节数
    uint64_t total_bytes = 0;
    uint64_t total_us    = 0;
};
```

---

## 改动点

### 1. SegmentWriter

**`flush()` 返回值**：`void` → `SegmentWriteStats`

**`writeTim()` 内部**：在遍历 term 时同步收集 df、max_tf_norm 统计，填入临时聚合变量，写完后回填到 stats。

**`writeDoc()` 内部**：`PForDelta::compress()` 返回 `skip_nodes`，每个 term 的 `skip_nodes.size()` 即该 term 的 skipnode 数，在遍历时累加到聚合统计。

**文件大小**：每个 `writeX()` 完成后用 `std::filesystem::file_size(path(...))` 取得精确值，回写到 stats 对应字段。

**写入计时**：在 `flush()` 中用 `std::chrono::steady_clock` 包围每个 `writeX()` 调用，精度微秒。

```cpp
// flush() 中的计时模式
using Clock = std::chrono::steady_clock;
auto t = Clock::now();
writeTim(mem_index, term_dict, stats);
stats.tim_us = std::chrono::duration_cast<std::chrono::microseconds>(
    Clock::now() - t).count();
stats.tim_bytes = std::filesystem::file_size(path("tim"));
```

**`writeTim()` 签名扩展**：增加 `SegmentWriteStats&` 引用参数，用于在遍历 term 时收集聚合统计（df、skip_node count）。

**`writeDoc()` 签名扩展**：同上，收集 skip_node 聚合统计。

### 2. FastFieldWriter

**`flush()` 返回值**：`void` → `FFWriteStats`

内部按字段统计 `std::filesystem::file_size(ffPath(...))` 和写入耗时。

### 3. IndexWriter

**`flush()` 内部**：
- 接收 `SegmentWriteStats` from `seg_writer.flush()`
- 接收 `FFWriteStats` from `ff_writer_.flush()`
- 调用 `printFlushStats(seg_stats, ff_stats)` 打印

**`printFlushStats()`**（静态私有方法）：格式化输出，见下方打印格式。

---

## 统计收集时机

| 统计项 | 收集位置 | 方法 |
|--------|---------|------|
| term_count | writeTim 结束 | `terms.size()` |
| df per term | writeTim 遍历中 | `pl->size()` |
| total / max / avg df | writeTim 遍历中累加 | 在线统计 |
| skip_node 数 | writeDoc 遍历中 | `skip_nodes.size()` |
| total / max / avg skip | writeDoc 遍历中累加 | 在线统计 |
| .tim 大小 | writeTim 返回后 | `fs::file_size(path("tim"))` |
| .doc 大小 | writeDoc 返回后 | `fs::file_size(path("doc"))` |
| .pos 大小 | writePos 返回后 | `fs::file_size(path("pos"))` |
| .fdt/.fdx 大小 | writeFdt/Fdx 后 | `fs::file_size(...)` |
| .liv/.si 大小 | writeLiv/Si 后 | `fs::file_size(...)` |
| .ff_* 大小 | ff_writer_.flush() 内部 | `fs::file_size(ffPath(...))` |
| 各写入耗时 | flush() 中包围 writeX | `steady_clock` 差值 |

---

## 打印格式

```
╔══════════════════════════════════════════════════════════════╗
║  Segment _2 — Build Stats                                    ║
╠══════════════════════════════════════════════════════════════╣
║  Docs:  1024   Terms:  567                                   ║
╠═══════════════ Posting List ═════════════════════════════════╣
║  Total entries:  18432  (avg  32.5 / term)                   ║
║  Max df     :    1024   term="python"                        ║
║  Skip nodes :     144   total  (avg  0.25 / term)            ║
║  Max skip   :       8   term="python"                        ║
╠═══════════════  File Sizes  ═════════════════════════════════╣
║  .tim        :    45.2 KB   [  2.3 ms]                       ║
║  .doc        :   123.4 KB   [  5.1 ms]                       ║
║  .pos        :   201.7 KB   [  8.7 ms]                       ║
║  .fdt + .fdx :   894.3 KB   [ 12.4 ms]                       ║
║  .liv        :     0.1 KB                                    ║
║  .si         :     0.1 KB                                    ║
║  .ff_pubtime :     8.0 KB   ]                                ║
║  .ff_uid     :     8.0 KB   } ff total:  1.2 ms              ║
║  .ff_page_rank:    4.0 KB   ]                                ║
╠═══════════════  Totals  ═════════════════════════════════════╣
║  Total size  :  1285.0 KB                                    ║
║  Total time  :    29.7 ms                                    ║
╚══════════════════════════════════════════════════════════════╝
```

---

## 实施顺序

1. **`fast_field_writer.h/.cpp`**：新增 `FFWriteStats` 结构体；`flush()` 改返回 `FFWriteStats`
2. **`segment_writer.h`**：新增 `SegmentWriteStats` 结构体；`flush()` 改返回 `SegmentWriteStats`；`writeTim`/`writeDoc` 增加 `SegmentWriteStats&` 参数
3. **`segment_writer.cpp`**：
   - `writeTim()` 遍历中收集 df 聚合
   - `writeDoc()` 遍历中收集 skip_node 聚合
   - `flush()` 添加 `steady_clock` 计时 + `file_size` 采集 + 组装 stats 并返回
4. **`index_writer.h/.cpp`**：接收两个 stats，实现 `printFlushStats()` 并在 `flush()` 中调用

---

## 关于 top-N per-term 展示

聚合统计（总和、均值、最大值 + 对应 term）已能定位异常。如需进一步诊断（哪些 term 的 posting list 异常大），可在设计阶段选配"TopK term by df"列表，默认不开启，由宏 `II_STATS_VERBOSE` 控制：

```cpp
#ifdef II_STATS_VERBOSE
    // 收集 top-5 by df，打印详细 term 列表
#endif
```

本次默认不实现，后续按需加入。

---

## 实际改动记录

### 改动文件汇总

| 文件 | 改动类型 | 关键内容 |
|------|---------|---------|
| `include/fastfield/fast_field_writer.h` | 新增结构体 + 签名变更 | `FFWriteStats` 结构体；`flush()` 返回类型 `void→FFWriteStats` |
| `src/fastfield/fast_field_writer.cpp` | 逻辑改造 | 写每个 FF 文件后用 `fs::file_size()` 采大小；`steady_clock` 计总耗时；返回 `FFWriteStats` |
| `include/segment/segment_writer.h` | 新增结构体 + 签名变更 | `SegmentWriteStats` 结构体（含 `totalSegBytes()`/`totalSegUs()` 辅助方法）；`flush()` 返回 `SegmentWriteStats`；`writeTim`/`writeDoc` 增 `SegmentWriteStats&` 参数 |
| `src/segment/segment_writer.cpp` | 逻辑改造 | `writeTim()` 遍历中累积 df 聚合（`total_pl_entries`/`max_pl_df`/`max_pl_term`）；`writeDoc()` 遍历中累积 skipnode 聚合；`flush()` 用 `steady_clock` 包围每个 `writeX()`，用 `fs::file_size()` 采文件大小，末尾计算均值字段，返回 `SegmentWriteStats` |
| `include/core/index_writer.h` | 新增声明 | `static void printFlushStats(const SegmentWriteStats&, const FFWriteStats&)` |
| `src/core/index_writer.cpp` | 逻辑改造 + 新增实现 | `flush()` 接收两个 stats 对象并调用 `printFlushStats()`；`printFlushStats()` 用 `printf` 格式化输出边框表格 |

### 设计偏差说明

**设计中的 `.liv`/`.si` 大小显示为 0.0 KB**：这两个文件实际只有几十字节（`.liv` 写位图，`.si` 写 4 个字段），`file_size()` 返回正确值但 KB 精度下显示 `0.0`。属预期现象，无需修正。

**FF 文件顺序**：`FFWriteStats.file_bytes` 用 `std::map` 存储，输出按字段名字母序排列，与设计图示顺序略有不同，但信息完整。

**设计中 `writeTim`/`writeDoc` 签名**：改为传 `SegmentWriteStats&` 引用（而非返回值），避免在每个子函数中重复传递已有的 stats 对象。`writePos`/`writeFdt`/`writeFdx`/`writeLiv`/`writeSi` 无需收集聚合数据，签名不变，计时和 `file_size` 在 `flush()` 中统一处理。

### 实际打印效果（256 docs / 2 segments 场景）

```
╔══════════════════════════════════════════════════════════════╗
║  Segment _0 — Build Stats                                   ║
╠══════════════════════════════════════════════════════════════╣
║  Docs:   218   Terms:   167                              ║
╠═══════════════ Posting List ═════════════════════════════════╣
║  Total entries :    9681  (avg   58.0 / term)            ║
║  Max df        :     218  term="comprehensive"       ║
║  Skip nodes    :     177  total  (avg  1.06 / term)         ║
║  Max skip nodes:       2  term="comprehensive"       ║
╠═══════════════  File Sizes  ═════════════════════════════════╣
║  .tim        :      7.6 KB  [  0.87 ms]                  ║
║  .doc        :     11.3 KB  [  3.46 ms]                  ║
║  .pos        :    163.0 KB  [  2.21 ms]                  ║
║  .fdt + .fdx :    216.0 KB  [  0.90 ms]                  ║
║  .liv        :      0.0 KB                           ║
║  .si         :      0.0 KB                           ║
║  .ff_page_rank:      0.9 KB  } ff total:  0.22 ms      ║
║  .ff_pubtime  :      1.7 KB                           ║
║  .ff_uid      :      1.7 KB                           ║
╠═══════════════  Totals  ═════════════════════════════════════╣
║  Total size  :    402.2 KB                           ║
║  Total time  :     7.66 ms                           ║
╚══════════════════════════════════════════════════════════════╝
```
