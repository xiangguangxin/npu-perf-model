# Phase 1 (MVP-1) 数据流程图

> 对应实现：`common.h` / `memory.h` / `onchip_buffer.h` / `dma_engine.h` /
> `workload_driver.h` / `perf_monitor.h` / `main.cpp`。
> 抽象层次：**LT（loosely-timed）**，单 DMA 串行搬运，先把数据流跑通；
> Memory / DMA 升级为 AT 是 MVP-3 的事，PE Array 是 MVP-2 的事。

MVP-1 只建 **访存侧数据流**：把一个 GEMM(M×K×N) 需要的三个操作数
（权重 M·K、激活 K·N、输出 M·N 字节）按 tile 大小分块，经**单个 DMA**
从 HBM 读进片上 buffer，测出访存时间 `T_memory`。**不含 PE 计算**。

---

## 1. 模块拓扑与绑定

`main.cpp` 组装的拓扑（MVP-1 特意跳过 Interconnect，DMA 直连 Memory）：

```mermaid
flowchart LR
    Driver["WorkloadDriver<br/><small>SC_THREAD run()</small><br/>tiling + 调度"]
    DMA["DmaEngine<br/><small>LT initiator</small><br/>isock"]
    MEM["Memory (HBM)<br/><small>LT target</small><br/>tsock"]
    BUF["OnchipBuffer<br/><small>容量 + 带宽约束</small>"]

    Driver -- "dma->read(addr,bytes,kind)" --> DMA
    DMA -- "b_transport(gp, delay)<br/>isock.bind(tsock)" --> MEM
    Driver -- "can_hold / allocate<br/>access_time / release" --> BUF

    classDef mod fill:#1f6feb22,stroke:#1f6feb,stroke-width:1px;
    class Driver,DMA,MEM,BUF mod;
```

- **实线 TLM 路径**：只有 `DMA.isock → Memory.tsock` 一条（`dma.isock.bind(mem.tsock)`）。
- **Buffer 不挂 socket**：它是纯 timing 辅助模块，由 Driver 直接函数调用查询占用与访问时间。
- **Driver 是唯一的 `SC_THREAD`**：整个仿真由它推进时间，跑完 `sc_stop()`。

---

## 2. 顶层调度：三个操作数依次流式搬运

`WorkloadDriver::run()` 的宏观流程：

```mermaid
flowchart TD
    A["run() 开始<br/>t0 = sc_time_stamp()"] --> B["stream_operand(M·K, WEIGHT)"]
    B --> C["stream_operand(K·N, ACTIVATION)"]
    C --> D["stream_operand(M·N, OUTPUT)"]
    D --> E["run_time = now - t0"]
    E --> F["sc_stop()"]
    F --> G["PerfMonitor::report(...)"]

    classDef step fill:#2da44e22,stroke:#2da44e;
    class A,B,C,D,E,F,G step;
```

> MVP-1 搬的是**理想最小字节 Bytes_min = (M·K + K·N + M·N)·data_bytes**
> （每个操作数只搬一遍，无复用惩罚）——这是访存下界的基准，复用建模留待后续。

---

## 3. 单个操作数内部：按 tile 分块的搬运循环

`stream_operand(total_bytes, kind)`，块大小 `tile_bytes = array_n² · data_bytes`：

```mermaid
flowchart TD
    S["remaining = total_bytes"] --> Chk{"remaining > 0 ?"}
    Chk -- 否 --> Done["本操作数搬完"]
    Chk -- 是 --> Sz["chunk = min(remaining, tile_bytes)"]
    Sz --> Hold{"buf.can_hold(chunk) ?"}
    Hold -- 否 --> Rel0["buf.release(全部)<br/><small>MVP-1 逐块进出，不建复用</small>"]
    Rel0 --> Alloc
    Hold -- 是 --> Alloc["buf.allocate(chunk)"]
    Alloc --> Read["dma->read(0, chunk, kind, tid++)<br/><small>阻塞：含 HBM 延迟 + 带宽时间</small>"]
    Read --> Wb["wait(buf.access_time(chunk))<br/><small>写入 buffer 的带宽时间</small>"]
    Wb --> Rel["buf.release(chunk)"]
    Rel --> Dec["remaining -= chunk"]
    Dec --> Chk

    classDef step fill:#1f6feb22,stroke:#1f6feb;
    class S,Sz,Alloc,Read,Wb,Rel,Dec,Rel0 step;
```

---

## 4. 一次 DMA 搬运的 LT 时序（关键 timing 注入点）

`dma->read()` → `Memory::b_transport()` 的一次事务，时间怎么被"记账"：

```mermaid
sequenceDiagram
    participant D as WorkloadDriver
    participant M as DmaEngine
    participant H as Memory (HBM)

    D->>M: read(addr, bytes, kind)
    Note over M: 构造 tlm_generic_payload<br/>挂 TileExtension(kind)
    M->>H: b_transport(gp, delay=0)
    Note over H: delay += hbm_lat_cyc(cycle)<br/>delay += bytes / hbm_bw
    H-->>M: 返回 (delay = 访问耗时), TLM_OK
    M->>M: wait(delay)  ← 真正推进仿真时间
    Note over M: bytes_moved += bytes<br/>num_xfers += 1
    M-->>D: read() 返回
    D->>D: wait(buf.access_time)  ← 再叠加 buffer 写入时间
```

**要点**：LT 里 `b_transport` 只是把"这次访问要花多久"累加进 `delay`，
是 `DmaEngine` 在 `wait(delay)` 时才把时间真正走掉。功能上不搬真实数据
（`data_ptr` 指向哑缓冲），这正是**性能模型**区别于功能模型之处——只关心"多快"。

---

## 5. 单块 tile 的时间构成（默认配置对账）

默认 `array_n=16, data_bytes=1 → tile=256B`；`hbm_bw=256GB/s, hbm_lat=100cyc@1GHz, buf_bw=64B/cyc`：

| 分量 | 公式 | 默认值 |
|------|------|--------|
| HBM 固定延迟 | `hbm_lat_cyc / freq` | 100 ns |
| HBM 传输 | `bytes / hbm_bw` = 256 / 256e9 | 1 ns |
| Buffer 写入 | `ceil(bytes / buf_bw_Bpc)` cyc = ceil(256/64)=4 | 4 ns |
| **单块合计** | | **105 ns** |

`sanity_tests` 里 16³ workload（每操作数正好 1 块、共 3 块串行）验证
`run_time == 3 × 105 = 315 ns`，与手算逐拍吻合。

> **MVP-1 暴露的洞察**：单 DMA + LT 串行，每块都被 100cyc 固定延迟卡死、无法隐藏，
> 512³ 实测有效带宽仅 ~2.4 GB/s(峰值 256)。这正是 **MVP-3 升级 AT + 多 outstanding
> 事务隐藏延迟**的动机。

---

## 6. 与后续阶段的关系

```mermaid
flowchart LR
    subgraph P1["Phase-1 (本图, 已完成)"]
        direction LR
        d1["Driver→DMA→Memory<br/>LT 数据流"] --- b1["Buffer 容量/带宽"]
    end
    P1 --> P2["Phase-2: + PE Array timing<br/>+ double buffering overlap<br/>→ 吞吐/利用率/AI"]
    P2 --> P3["Phase-3: DMA↔Memory 升级 AT<br/>(4-phase + PEQ)"]
    P3 --> P4["加分: + Interconnect 仲裁<br/>→ contention 实验"]

    classDef done fill:#2da44e22,stroke:#2da44e;
    classDef todo fill:#8957e522,stroke:#8957e5;
    class P1 done;
    class P2,P3,P4 todo;
```
