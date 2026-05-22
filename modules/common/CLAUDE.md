# common 模块

## 职责
并发与基础工具：多线程索引构建所需的阻塞队列、线程池、零拷贝文档引用、
文件操作工具、泛型 K-way 合并迭代器。不含任何业务逻辑。

## 对外接口
- `BlockingQueue<T>(capacity)` — 有界阻塞队列，MPMC，close() 通知退出
- `ThreadPool(n_threads)` — 固定线程数任务池，submit() / waitAll()
- `DocRef` — `std::unique_ptr<Document>` 别名，零拷贝移动语义
- `file_utils::ensureDir / listSegmentIds / deleteSegmentFiles`
- `KwayMerge<T, Compare>` — 泛型 K-way 最小堆合并迭代器

## 关键约定
- BlockingQueue::close() 是广播关闭信号，pop() 在队列空后返回 false
- push() 在队列关闭后返回 false，调用方不应继续写入
- ThreadPool::waitAll() 等待所有已 submit() 的任务完成
- DocRef 是 unique_ptr，传递时必须 std::move

## 依赖
core/（仅 DocRef 引用 Document 类型）
