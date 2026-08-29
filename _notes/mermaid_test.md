# Mermaid 渲染测试（诊断用）

> 如果这个最简单的图在预览里都是黑的，说明是 VS Code 预览环境/扩展的问题，而不是图的问题。

```mermaid
flowchart LR
    A[内核] --> B[调度器]
    B --> C[CFS]
```

如果上面能正常显示，再试下面这个"子图+中文"版本：

```mermaid
flowchart TB
    subgraph K["内核核心"]
        A["进程管理"]
        B["内存管理"]
    end
    K --> C["硬件"]
```
