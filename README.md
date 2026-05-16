# Inverted Index — C++17 实现

用 C++17 从零实现的倒排索引引擎。

## 工程结构

```
piggy_engine/
├── CMakeLists.txt
├── README.md
├── include/                        ← 所有头文件（按模块分目录）
│   ├── types.h                     基础类型（DocId、Token、TermMeta 等）
│   ├── core/
│   │   └── index_writer.h          文档写入入口
│   ├── query/
│   │   └── index_searcher.h        AND（Zigzag）/ OR（WAND）查询
│   ├── postings/
│   │   ├── pfor_delta.h            PForDelta 压缩 / 解压
│   │   ├── skiplist.h              Block 级跳表（Level0 + Level1）
│   │   └── posting_list.h          内存倒排链
│   ├── segment/
│   │   ├── segment_writer.h        Flush → Segment 文件
│   │   ├── segment_reader.h        只读打开 Segment
│   │   └── segment_merger.h        Segment 合并 + 软删除
│   ├── tokenizer/
│   │   └── analyzer.h              文本分析管道（CharFilter→Tokenizer→Stem）
│   ├── fastfield/                  （预留）列存 / 属性字段
│   ├── store/                      （预留）原文存储压缩
│   ├── collector/                  （预留）结果收集器
│   ├── positions/                  （预留）位置信息
│   └── query-grammar/              （预留）自定义查询 DSL
├── src/                            ← 实现文件（与 include/ 同结构）
│   ├── core/
│   │   ├── index_writer.cpp
│   │   └── main.cpp                256 篇文档 Demo
│   ├── query/
│   │   └── index_searcher.cpp
│   ├── postings/
│   │   ├── pfor_delta.cpp
│   │   ├── skiplist.cpp
│   │   └── posting_list.cpp
│   ├── segment/
│   │   ├── segment_writer.cpp
│   │   ├── segment_reader.cpp
│   │   └── segment_merger.cpp
│   └── tokenizer/
│       └── analyzer.cpp
└── tests/
    └── test_all.cpp                单元测试（无第三方依赖）
```

## 核心模块说明

| 模块 | 对应原理 | 关键实现 |
|------|---------|---------|
| `analyzer` | CharFilter→Tokenizer→StopFilter→StemFilter | 简化 Porter Stemmer |
| `pfor_delta` | Delta 编码 + 选位宽 + 补丁区 | bit 级紧凑打包，支持任意位宽 |
| `skiplist` | Block 级 SkipList，Level0+Level1 | 序列化 / 反序列化，O(log N) 查找 |
| `posting_list` | 内存倒排链 | 追加同一 doc 时累加 tf |
| `segment_writer` | Flush → .tim/.doc/.pos/.fdt/.fdx/.liv/.si | 顺序写，回填 offset |
| `segment_reader` | 按需 seek 读取，模拟 mmap | .tim 全量加载，.doc 按需 seek |
| `index_writer` | RAM Buffer + 自动 flush | 估算内存，超阈值触发 flush |
| `index_searcher` | AND=Zigzag，OR=WAND TopK | 跨 Segment 归并 |

## 编译运行

在**项目根目录**执行以下命令：

```bash
# 第一步：生成构建系统（只需执行一次，或 CMakeLists.txt 变更后重新执行）
# -S .              源码目录为当前目录
# -B build          构建产物输出到 build/，不污染源码目录
# -DCMAKE_BUILD_TYPE=Release  开启 -O2 优化
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 第二步：编译（-j4 表示使用 4 个并行线程）
cmake --build build -j4

# 运行 Demo（写入 256 篇文档 + 搜索）
./build/demo

# 运行单元测试
./build/test_all
```

调试模式（带调试符号，关闭优化）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
```

后续只改代码，无需重新 cmake，直接：

```bash
cmake --build build -j4
```

## 生成的 Segment 文件

```
/tmp/ii_demo_index/
├── _0.si      Segment 元数据
├── _0.tim     Term 词典（所有 term 的 df/UB/offset）
├── _0.doc     Posting List（SkipList + PForDelta Block）
├── _0.pos     位置信息（短语查询用）
├── _0.fdt     文档原文（title/body/category）
├── _0.fdx     文档存储索引（doc_id → fdt 偏移）
├── _0.liv     存活文档位图（软删除标记）
├── _1.*       第二个 Segment（RAM buffer 满时自动生成）
└── segments_N Segment 注册表
```

## 关键设计对照

| 原理 | 本代码实现 |
|------|-----------|
| Block 大小 128 | `PForDelta::BLOCK_SIZE = 128` |
| SkipList 节点存 byte_offset | `SkipNode::byte_offset` |
| Header 固定 16 byte | `BlockHeader`（`#pragma pack(push,1)`）|
| 补丁区每条 6 byte | `PatchEntry`（2B 位置 + 4B 值）|
| .tim 全量加载内存 | `SegmentReader::loadTim()` |
| .doc 按需 seek | `doc_file_.seekg(meta->posting_offset)` |
| 软删除用 .liv 位图 | `SegmentReader::softDelete()` |
| AND = Zigzag 交集 | `IndexSearcher::searchAND()` |
| OR = WAND TopK | `IndexSearcher::searchOR_WAND()` |

## 与主流搜索引擎对比

### 核心索引结构

| 特性 | 本实现 | Lucene/ES | Tantivy | Pisa | Havenask | Vespa |
|------|--------|-----------|---------|------|----------|-------|
| 实现语言 | C++17 | Java | Rust | C++ | C++ | C++ |
| Term 词典 | `std::map` | FST | FST | 哈希/排序数组 | Trie + 哈希 | FST |
| Posting List 压缩 | PForDelta | PForDelta | BlockWAND | Binary Interpolative / PEF | 自定义压缩 | PForDelta |
| SkipList | Block 级两层 | 多层跳表 | 无，依赖 Block Header | Block 级 | 多层跳表 | Block 级 |
| 位置信息 | `.pos` 独立文件 | `.pos` + `.pay` | `positions` 嵌 Block | 不存位置 | 独立 `pos` 文件 | 嵌入 Block |
| 原文存储 | `.fdt/.fdx` | `.fdt/.fdx` LZ4 压缩 | Zstd 压缩 | 不存原文 | 多列独立文件 | 列存 |
| 软删除 | `.liv` 位图 | `.liv` 位图 | `DeleteBitSet` | 不支持 | `.delvec` | 文档级版本号 |

### 查询执行

| 特性 | 本实现 | Lucene/ES | Tantivy | Pisa | Havenask | Vespa |
|------|--------|-----------|---------|------|----------|-------|
| AND 算法 | Zigzag 交集 | Zigzag | Zigzag | BlockMaxWAND | Zigzag | Zigzag + WAND |
| OR Top-K 算法 | WAND（UB 剪枝）| BlockMaxWAND | WAND | BlockMaxWAND / VBMW | WAND | BlockMaxWAND |
| Posting 迭代 | 全量解压到 vector | 惰性迭代器（DISI）| 惰性迭代器 | 惰性迭代器 | 惰性迭代器 | 惰性迭代器 |
| BM25 | 近似（无实际 doc len）| 完整 BM25F | BM25 | BM25 | BM25 + 自定义 | BM25 + 神经网络 |
| 向量检索 | 未实现 | HNSW（后加）| 未内置 | 未实现 | ANN（Proxima）| HNSW 原生支持 |
| 多字段 | 未实现 | 完整 | 完整 | 单字段 | 多字段 | 多字段 + 结构化 |

### 工程能力

| 特性 | 本实现 | Lucene/ES | Tantivy | Pisa | Havenask | Vespa |
|------|--------|-----------|---------|------|----------|-------|
| 分布式 | 无 | ES 协调层 | 无（库）| 无（库）| QRS + Searcher 分层 | 原生分布式 |
| 实时写入 | 内存 flush | NRT（近实时）| NRT | 离线批量 | 实时 build_service | 实时 |
| Segment Merge | 单线程同步 | 后台异步分级 | 后台异步 | 不支持 | 后台异步 | 后台异步 |
| mmap | `seekg` 模拟 | 真正 `mmap()` | `mmap()` | `mmap()` | `mmap()` | `mmap()` |
| SIMD 解压 | 位循环 | AVX2/AVX512 | SIMD 加速 | SIMD 加速 | SIMD 加速 | AVX2 |
| 在线 ML 排序 | 无 | 插件（LTR）| 无 | 无 | 表达式引擎 | ONNX 原生推理 |

### 各引擎定位与学习价值

| 引擎 | 定位 | 对本项目的参考价值 |
|------|------|----------------|
| **Lucene/ES** | 工业全功能标准 | FST 词典、DocValues 列存、分布式架构 |
| **Tantivy** | Lucene 的 Rust 重写，代码最干净 | `BlockSegmentPostings` 惰性迭代器、Segment 合并流程 |
| **Pisa** | 学术算法库，每个优化对应论文 | `block_max_wand_query` WAND/BMW 完整实现 |
| **Havenask** | 阿里电商大规模搜索，C++ 高性能 | 列存 + 倒排融合、实时索引构建（build_service）|
| **Vespa** | 向量 + 倒排 + 结构化真正融合 | WAND 工业实现、在线 ML 推理嵌入排序阶段 |

### 本实现的当前缺口

基于上表，与工业标准差距最大的两处：

1. **Posting 迭代器**：当前全量解压到 `vector`（内存 O(N)），参考 Tantivy `BlockSegmentPostings` 改为惰性迭代（内存 O(1)），`SkipList` 和 `upper_bound` 字段已就位，缺查询执行层串联。

2. **Top-K 剪枝**：`TermMeta::upper_bound` 和 `SkipNode::max_score` 已写入文件，但查询时未消费，参考 Pisa `block_max_wand_query` 实现 BlockMaxWAND，理论上可跳过 90%+ 无效 Block。
