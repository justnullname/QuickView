# QuickView Bug 评估报告: Issue #85

## 1. 核实问题是否真实存在

经过对代码库的分析，用户在 Issue #85 中报告的问题是**真实存在的**。

**问题重现路径分析：**
在 `HeavyLanePool.cpp` 的 `HeavyLanePool::WorkerLoop` 函数中（约第850-900行），Worker 会申请 IO 信号量 (`m_ioSemaphore.acquire()`) 以限制硬盘读取的并发量。
```cpp
// 原始代码：
bool acquiredIO = false;
if (!m_isTitanMode) {
    m_ioSemaphore.acquire();
    acquiredIO = true;
}
```

紧接着，代码会进行一系列状态检查（如 `!st.stop_requested()`，即是否被取消）。如果在此时发现任务已经被取消（例如用户在进行快速翻页，触发了 `CancelOthers` 方法请求取消任务），或者任务对应区块不再可见，代码会直接进入错误处理分支，并正确地释放信号量 (`if (acquiredIO) m_ioSemaphore.release();`)。

然而，**如果这些前置检查都通过了，程序将调用 `PerformDecode(...)` 进行实际解码。**
```cpp
// 原始代码：
PerformDecode(workerId, job, st, &self.loaderName);

if (acquiredIO) m_ioSemaphore.release();
```

`PerformDecode` 内部是一个复杂的解码流程，且接受了 `std::stop_token st`。如果用户在 `PerformDecode` 执行的这几百毫秒到几秒的时间窗口内进行了快速翻页（调用了 `CancelOthers` 触发了 `request_stop()`），`PerformDecode` 内部可能会抛出异常（或者在 C++ 标准异常导致直接跳出函数时），那么紧随其后的 `m_ioSemaphore.release()` 将**永远不会被执行**。此外，如果在 `PerformDecode` 内部遇到严重错误或抛出异常跳出当前 `WorkerLoop` 迭代，也会导致信号量丢失。

当信号量泄漏的次数达到最大并发限制 (`maxHeavyWorkers`) 时，`m_ioSemaphore` 将永久归零，导致后续所有新的图像解码任务都阻塞在 `m_ioSemaphore.acquire()`，从而表现为“解码永远不再启动”的死锁状态。

## 2. 评估 Bug 汇报的修复方案

**用户提出的修复方案：**
用户建议使用基于 RAII (Resource Acquisition Is Initialization) 机制的自动资源管理，创建一个类似 `ScopedIOSlot` 的结构体，在构造时获取信号量，析构时自动释放信号量。

**评估：**
- **是否有效？** 是的，极其有效。RAII 是 C++ 中处理此类资源泄漏（尤其是异常安全或早期返回导致的泄漏）的标准且最可靠的做法。
- **是否最佳修复方案？** 是的，由于它利用了 C++ 的作用域生命周期，无论 `PerformDecode` 是正常返回、提前返回还是抛出异常，栈上分配的 `ScopedIOSlot` 对象都会被销毁，从而确保析构函数中的 `m_ioSemaphore.release()` 被绝对执行。
- **是否有更好更彻底的方案？** 用户的方案已经切中要害。为了完美契合 QuickView 的特殊逻辑（即“Titan Mode 内存映射模式跳过 IO 限制”），我们可以将该模式检查集成到 RAII 结构体中，使得外层调用更加整洁和安全。

## 3. 实施的修复细节

在修复此 Bug 时，我采纳了基于 RAII 的思路，并直接在 `HeavyLanePool::WorkerLoop` 函数内部定义了一个局部结构体 `ScopedIOSlot`：

```cpp
struct ScopedIOSlot {
    std::counting_semaphore<>& sem;
    bool acquired = false;
    explicit ScopedIOSlot(std::counting_semaphore<>& s, bool isTitanMode) : sem(s) {
        if (!isTitanMode) {
            sem.acquire();
            acquired = true;
        }
    }
    ~ScopedIOSlot() { if (acquired) sem.release(); }
};

ScopedIOSlot ioSlot(m_ioSemaphore, m_isTitanMode);
```

**改进点：**
将 `m_isTitanMode` 检查直接纳入了 `ScopedIOSlot` 构造函数中，确保“非 Titan 模式才申请，申请了才释放”的逻辑在一个原子结构中闭环管理。同时，去除了原来代码中散落在错误分支 (`if (!stillValid)`) 以及结尾的 `if (acquiredIO) m_ioSemaphore.release();` 冗余代码。

这不仅**彻底解决了由于异常或取消导致的信号量泄漏问题**，同时也简化了 Worker 主循环的代码复杂度，提高了代码的可维护性和异常安全性。
