# Tools

本目录包含基于 piggy_engine 核心库构建的命令行工具。

---

## 编译

所有工具通过根目录的 CMake 统一编译，产物输出到 `build/` 目录。

```bash
# 在项目根目录执行
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target wiki_indexer wiki_searcher
```

或一次编译所有目标：

```bash
cmake --build build
```

---

## wiki_indexer

遍历 Wikipedia JSONL 数据目录，对每篇文章的 `text` 字段建倒排索引，构建完成后打印每个 term 的 posting 详情。

### 数据格式

每个文件为 JSON Lines 格式，每行一个 JSON 对象：

```json
{"id": "12", "title": "Anarchism", "text": "Anarchism is a political philosophy..."}
```

### 用法

```
./build/wiki_indexer --input <wiki_dir> [options]
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--input <path>` | Wiki JSONL 数据目录（递归遍历）| 必填 |
| `--output <path>` | 索引输出目录 | `./wiki_index` |
| `--ram <MB>` | IndexWriter RAM buffer 上限，超出自动 flush | `128` |
| `--limit <N>` | 最多索引 N 篇文档（调试用） | 不限 |
| `--top <N>` | 打印 posting 详情的 term 数（按 df 降序） | `50` |
| `--verbose` | 同时打印 top-N 之外所有 term 的 df/ttf/UB（输出量大，建议重定向到文件） | 关闭 |

### 示例

```bash
# 索引全量数据，RAM buffer 256MB，打印 top 30 term
./build/wiki_indexer \
  --input /path/to/wiki_data/text \
  --output ./wiki_index \
  --ram 256 \
  --top 30

# 只索引前 100 篇，快速验证
./build/wiki_indexer \
  --input /path/to/wiki_data/text \
  --output ./wiki_index_test \
  --limit 100 \
  --top 20

# 导出所有 term 的 posting 信息到文件
./build/wiki_indexer \
  --input /path/to/wiki_data/text \
  --output ./wiki_index \
  --top 0 --verbose > postings.txt
```

### 输出示例

```
[Build] Found 373 file(s) in /path/to/wiki_data/text
[Build] Docs indexed    : 50000
[Build] Total time      : 142.3s
[Build] Throughput      : 351 docs/s

======================================================================
  Index Statistics
======================================================================
  Segments         : 3
  Total docs       : 50000
  Unique terms     : 412873
  Total term occurrences (ttf sum): 28473651

----------------------------------------------------------------------
  Top 30 terms by document frequency (with posting sample)
----------------------------------------------------------------------
Rank  Term                        df      ttf       UB      Posting sample
----------------------------------------------------------------------
1     also                        48921   312450    0.143   [1, 2, 3, 4, 5 ...]
2     first                       47832   201234    0.151   [1, 2, 3, 4, 5 ...]
...

  (412843 more terms omitted — use --verbose to show all)
```

---

## wiki_searcher

对 `wiki_indexer` 构建的索引执行全文检索，返回匹配文章的 DocID 和标题。

支持单次查询和交互式 REPL 两种模式。

### 用法

```
./build/wiki_searcher --index <index_dir> [options]
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--index <path>` | 索引目录（`wiki_indexer` 的 `--output` 路径） | 必填 |
| `--query <text>` | 查询字符串，空格分隔多个词；不传则进入交互模式 | 交互模式 |
| `--mode AND\|OR` | `AND`：所有词必须出现；`OR`：任意词出现（WAND TopK） | `OR` |
| `--top <N>` | 返回前 N 个结果 | `10` |

### 示例

**单次查询：**

```bash
# OR 查询（默认），返回 top 10
./build/wiki_searcher \
  --index ./wiki_index \
  --query "anarchism political philosophy"

# AND 查询，所有词必须同时出现
./build/wiki_searcher \
  --index ./wiki_index \
  --query "machine learning neural" \
  --mode AND \
  --top 5
```

**交互模式：**

```bash
./build/wiki_searcher --index ./wiki_index --mode OR --top 10
```

```
[Searcher] Ready.
Interactive mode — type a query and press Enter. Ctrl-D or 'quit' to exit.
Mode: OR  Top: 10

>> anarchism
>> python programming language
>> :and anarchism political      # 行内临时切换为 AND 模式
>> :or  french revolution        # 行内临时切换为 OR 模式
>> quit
```

### 输出示例

```
[Query] "anarchism political philosophy"  mode=OR  hits=5
------------------------------------------------------------------------
Rank DocID     Score    Title (Wiki Article)
------------------------------------------------------------------------
1    1         3.9371   Anarchism
2    15        3.9371   Ayn Rand
3    7         1.8089   Aristotle
4    19        1.8089   List of Atlas Shrugged characters
5    6         1.8089   Abraham Lincoln
```

### 字段说明

| 字段 | 含义 |
|------|------|
| `DocID` | 索引内部顺序编号（建库时按文件遍历顺序分配，1-indexed） |
| `Score` | BM25 相关性得分（越高越相关） |
| `Title` | Wikipedia 文章标题（对应 JSON 的 `title` 字段） |

> **注意**：JSON 中的原始数字 `"id"` 字段（如 `"id": "12"`）在当前版本未存入索引。
> 如需通过 DocID 反查原始 wiki id，可在 `wiki_indexer.cpp` 中将其存入 `doc.category` 字段。
