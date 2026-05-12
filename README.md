# Inverted Index — C++17 实现

基于我们对话中讨论的所有原理，用 C++17 从零实现的倒排索引引擎。

## 工程结构

```
inverted_index/
├── CMakeLists.txt
├── README.md
├── include/                   ← 所有头文件
│   ├── types.h                基础类型（DocId、Token、TermMeta 等）
│   ├── analyzer.h             文本分析管道（CharFilter→Tokenizer→Stem）
│   ├── pfor_delta.h           PForDelta 压缩 / 解压
│   ├── skiplist.h             Block 级跳表（Level0 + Level1）
│   ├── posting_list.h         内存倒排链
│   ├── segment_writer.h       Flush → Segment 文件
│   ├── segment_reader.h       只读打开 Segment
│   ├── index_writer.h         文档写入入口
│   └── index_searcher.h       AND（Zigzag）/ OR（WAND）查询
├── src/                       ← 实现文件
│   ├── analyzer.cpp
│   ├── pfor_delta.cpp
│   ├── skiplist.cpp
│   ├── posting_list.cpp
│   ├── segment_writer.cpp
│   ├── segment_reader.cpp
│   ├── index_writer.cpp
│   ├── index_searcher.cpp
│   └── main.cpp               256 篇文档 Demo
└── tests/
    └── test_all.cpp           单元测试（无第三方依赖）
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

```bash
# 编译
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4

# 运行 Demo（写入 256 篇文档 + 搜索）
./demo

# 运行单元测试
./test_all
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

## 与 Lucene 的简化对比

| 特性 | 本实现 | Lucene |
|------|--------|--------|
| Term 词典 | `std::map` 二进制序列 | FST（有限状态转换机）|
| 原文压缩 | 无压缩 | LZ4 / Deflate Chunk |
| mmap | `ifstream::seekg` 模拟 | 真正 `mmap()` 系统调用 |
| Stemmer | 简化规则 | Snowball/Porter 完整实现 |
| SIMD 解压 | 位循环 | AVX2/AVX512 指令 |
| DocValues | 未实现 | 完整列存 |
