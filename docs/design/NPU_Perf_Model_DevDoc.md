# 项目 A 开发文档：SystemC TLM-AT 简化 NPU 性能模型

> 目标：用 SystemC TLM2.0（AT style）建一个 weight-stationary 脉动阵列 NPU 的**性能模型**，输入 GEMM workload，输出吞吐 / 延迟 / 利用率预测，并完成 roofline 分析、配置敏感性研究与 validation。
>
> 本文档包含：
> 0. **前置背景知识**（NPU / 脉动阵列 / Roofline / dataflow，开工前补）
> 1. MVP 的 SystemC 模块代码骨架
> 2. 脉动阵列 timing 公式的精确推导
> 3. 博客大纲与关键图清单
>
> **建议**：背景知识不必全部读完才开工，按下面"学习顺序"边读边搭最高效——读完 TPU + Roofline 两篇就能动手 MVP，其余与开发并行消化。

---

## 前置背景知识（开工前 / 开工中补）

> 这份清单不是泛泛的 NPU 科普，而是**精确对应你要建的东西**。每个知识点都标了它支撑项目的哪个模块 / 哪个实验。理解被建模的硬件是性能建模的基础——知道每个参数和简化背后的"为什么"，才能做出合理的抽象取舍，也能在讨论中清晰解释模型边界。

### B.1 脉动阵列（Systolic Array）—— 优先级最高，必须吃透

**支撑**：PE Array 模块（1.6）+ 全部 timing 公式（第 2 部分）。

要搞懂的：
- 脉动阵列为什么能高效做矩阵乘——数据在 PE 之间"脉动"流动、复用，避免反复访存。
- weight-stationary / output-stationary / input-stationary 三种 dataflow 的区别，各自把哪种数据"钉"在 PE 里。本项目选 **weight-stationary**。
- 为什么有 fill（填充）/ steady（稳态）/ drain（排空）三阶段——你公式里的 `2N`、`N` 就是从数据在阵列里的传播路径推出来的。

**必读**：Google TPU 论文 *"In-Datacenter Performance Analysis of a Tensor Processing Unit"*（Jouppi et al., ISCA 2017）。脉动阵列在工业界的标杆案例，用的正是 weight-stationary 大阵列，和你选型一致。读完你对"自己在建什么"会有质的清晰。

### B.2 Roofline 模型 —— 必须吃透

**支撑**：实验 1（Roofline 定位）+ validation + 博客中的关键图 3。

要搞懂的：
- arithmetic intensity（运算/访存比）这个横轴的物理含义。
- 为什么 roofline 是"斜线段（带宽屋檐）+ 水平段（算力屋顶）"两段。
- 拐点 `AI*` 怎么算、意味着什么；compute-bound vs memory-bound 怎么判定。

**必读**：*"Roofline: An Insightful Visual Performance Model for Multicore Architectures"*（Williams et al., CACM 2009）。短、经典、终身受用。

### B.3 数据复用与 dataflow（data reuse）—— 必须理解

**支撑**：实验 2（Buffer 敏感性）+ 公式 `Bytes_actual / reuse_penalty`（2.3）。

核心命题：**算力很便宜，搬数据很贵**。整个加速器架构都围绕"最大化片上复用、最小化 HBM 访问"设计。

要搞懂的：
- 为什么 on-chip SRAM 容量直接决定 tiling 大小，tiling 又决定复用率。
- temporal reuse vs spatial reuse。
- 为什么 memory-bound 是 NPU 上常见痛点。

**必读**：*"Efficient Processing of Deep Neural Networks: A Tutorial and Survey"*（Sze et al., Proc. IEEE 2017），以及 Eyeriss 论文。这篇 survey 是 NPU 入门"圣经"，dataflow / 复用 / 能效讲得最系统。**只读一篇深的，就读它。**

### B.4 NPU 整体架构构成 —— 建立全局图景（了解即可）

**支撑**：博客"抽象层次选择"段（3.1 第 2 节）+ "局限与展望"段。

了解真实 NPU 由哪些部件组成，才能说清你的简化模型省略了什么、是否合理：
- 典型组成：PE 阵列 + 多级片上 buffer（global buffer / 各级 SRAM）+ DMA + NoC/互联 + 控制器 + HBM 接口。
- 标量 / 向量 / 张量单元的分工。
- 真实 NPU 还要处理 activation、normalization、量化等非 GEMM 部分。

### B.5 AI workload 的计算特征 —— 你建模的输入（了解即可）

**支撑**：实验 1 里"有意识地"挑 GEMM 形状去打 roofline 不同段。

要了解的：
- GEMM 为什么是 NPU 核心——transformer、CNN 最后都归约到大量矩阵乘。
- attention、convolution 怎么映射成 GEMM。
- 不同算子 arithmetic intensity 差异很大，决定它们落在 roofline 哪段：大 GEMM 偏 compute-bound，element-wise / 小 batch 推理偏 memory-bound。

### 学习顺序（与开发并行，约一周）

| 顺序 | 读什么 | 解锁项目的什么 | 预计 |
|------|--------|---------------|------|
| 1 | TPU 论文 + Roofline 论文 | PE 公式 + 分析框架，读完即可搭 MVP | 2-3 天 |
| 2 | Sze survey 的 dataflow / reuse 章节（搭 MVP 同时读） | Buffer 敏感性实验的理论 | 3-4 天 |
| 3 | NPU 整体架构 + workload 特征 | 博客"抽象选择/局限"段 + 深入理解架构全景 | 2 天 |

> 阅读量约一周，和 MVP 第一阶段开发并行就消化掉了。不要"全部读完才开工"——读完第 1 项就开始搭。

---

## 第 0 部分 · 工程总览与开发顺序

**技术栈**：C++17 + SystemC 2.3.x + TLM2.0（Accellera 开源版）+ CMake

**抽象层次定位**：cycle-approximate（近似周期级），不做 cycle-accurate RTL，也不做纯解析。计算单元只建 timing 不做真实 MAC。

**推荐目录结构**：

```
npu-perf-model/
├── CMakeLists.txt
├── README.md
├── include/
│   ├── common.h          // 全局类型、配置结构、cycle<->time 换算
│   ├── pe_array.h
│   ├── onchip_buffer.h
│   ├── dma_engine.h
│   ├── interconnect.h
│   ├── memory.h
│   ├── workload_driver.h
│   └── perf_monitor.h
├── src/
│   └── (对应 .cpp，或 header-only)
├── tests/
│   └── sanity_tests.cpp
└── scripts/
    └── plot_results.py   // 读 CSV 出 roofline / 敏感性曲线
```

**开发顺序（防止陷进去）**：

| 阶段 | 内容 | 预计 |
|------|------|------|
| MVP-1 | common + Memory + Buffer + 单 DMA（LT 先跑通数据流） | 2-3 天 |
| MVP-2 | PE Array timing + double buffering overlap，输出吞吐/利用率 | 2-3 天 |
| MVP-3 | DMA↔Memory 路径升级为 **AT**（4 phases + PEQ） | ✅ 已完成 |
| MVP-4 | Interconnect/Arbiter + contention 实验 | 2 天 |
| MVP-5 | 解析模型交叉验证 + 敏感性扫描脚本 + 博客 | 3-4 天 |

先用 LT 把功能跑通，再把关键路径换 AT —— 不要一上来就写 AT，会卡在调试 phase 协议上。

---

## 第 1 部分 · MVP SystemC 模块代码骨架

> 以下是**骨架 + 伪实现**，关键 timing 接口写清楚，计算细节留 `// TODO` 给你填。重点看 socket 声明、timing 注入点、AT phase 流转。

### 1.1 common.h —— 全局定义

```cpp
#pragma once
#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/peq_with_cb_and_phase.h>
#include <cstdint>

using namespace sc_core;
using namespace tlm;

// ---- 时钟与换算 ----
constexpr double CLK_FREQ_HZ = 1.0e9;          // 1 GHz
inline sc_time cycles(uint64_t n) {            // cycle 数 -> sc_time
    return sc_time(double(n) / CLK_FREQ_HZ, SC_SEC);
}

// ---- 硬件配置（可从命令行/配置文件注入）----
struct NpuConfig {
    uint32_t array_n      = 16;        // 脉动阵列边长 N (N×N PE)
    uint32_t buffer_kb    = 256;       // 片上 buffer 容量
    double   buf_bw_Bpc   = 64;        // buffer 带宽 (Bytes per cycle)
    double   hbm_bw_GBps  = 256;       // HBM 带宽
    uint32_t hbm_lat_cyc  = 100;       // HBM 访问延迟 (cycle)
    uint32_t dma_outstanding = 4;      // DMA 最大未完成事务
    bool     double_buffer = true;     // 是否开启预取重叠
    uint32_t data_bytes   = 1;         // 每元素字节数 (int8=1)
};

// ---- GEMM workload 描述 ----
struct GemmTask { uint32_t M, K, N; };

// ---- 用扩展把"这是哪种数据/哪个 tile"挂在 payload 上 ----
struct TileExtension : tlm_extension<TileExtension> {
    enum Kind { WEIGHT, ACTIVATION, OUTPUT } kind;
    uint32_t tile_id = 0;
    tlm_extension_base* clone() const override { return new TileExtension(*this); }
    void copy_from(tlm_extension_base const& e) override { *this = static_cast<const TileExtension&>(e); }
};
```

### 1.2 memory.h —— HBM 抽象（AT target）

```cpp
#pragma once
#include "common.h"

struct Memory : sc_module {
    tlm_utils::simple_target_socket<Memory> tsock;
    NpuConfig cfg;
    // PEQ：把请求按延迟排队后回调，模拟访问延迟
    tlm_utils::peq_with_cb_and_phase<Memory> m_peq;

    SC_HAS_PROCESS(Memory);
    Memory(sc_module_name n, NpuConfig c)
      : sc_module(n), tsock("tsock"), cfg(c),
        m_peq(this, &Memory::peq_cb) {
        tsock.register_nb_transport_fw(this, &Memory::nb_fw);
    }

    // 接收请求：BEGIN_REQ 进来 -> 排 hbm_lat 后产生 BEGIN_RESP
    tlm_sync_enum nb_fw(tlm_generic_payload& gp, tlm_phase& ph, sc_time& delay) {
        if (ph == BEGIN_REQ) {
            // 带宽建模：传输占用时间 = size / 有效带宽
            double bytes = gp.get_data_length();
            sc_time xfer = sc_time(bytes / (cfg.hbm_bw_GBps * 1e9), SC_SEC);
            sc_time when = delay + cycles(cfg.hbm_lat_cyc) + xfer;
            m_peq.notify(gp, END_REQ, delay);          // 立刻 END_REQ 释放请求相位
            m_peq.notify(gp, BEGIN_RESP, when);        // 延迟后给响应
            return TLM_ACCEPTED;
        }
        if (ph == END_RESP) { return TLM_COMPLETED; }  // initiator 收完
        return TLM_ACCEPTED;
    }

    void peq_cb(tlm_generic_payload& gp, const tlm_phase& ph) {
        tlm_phase p = ph; sc_time d = SC_ZERO_TIME;
        gp.set_response_status(TLM_OK_RESPONSE);
        tsock->nb_transport_bw(gp, p, d);   // 把 END_REQ / BEGIN_RESP 发回 initiator
    }
};
```

> **要点**：Memory 同时建了 **延迟**（hbm_lat_cyc）和 **带宽**（size/bw）。这是性能模型和功能模型的根本区别 —— 功能模型只搬数据，性能模型要算"搬多久"。

### 1.3 interconnect.h —— 仲裁器（contention 来源，加分模块）

```cpp
#pragma once
#include "common.h"
#include <queue>

// 多个 DMA initiator 经此竞争通往 Memory 的单一通道
struct Interconnect : sc_module {
    tlm_utils::simple_target_socket<Interconnect>   tsock; // 朝 DMA 侧
    tlm_utils::simple_initiator_socket<Interconnect> isock; // 朝 Memory 侧
    NpuConfig cfg;
    bool channel_busy = false;
    std::queue<tlm_generic_payload*> wait_q;   // 排队中的请求
    sc_event chan_free_ev;

    SC_HAS_PROCESS(Interconnect);
    Interconnect(sc_module_name n, NpuConfig c)
      : sc_module(n), tsock("tsock"), isock("isock"), cfg(c) {
        tsock.register_nb_transport_fw(this, &Interconnect::nb_fw);
        isock.register_nb_transport_bw(this, &Interconnect::nb_bw);
        SC_THREAD(arbiter);
    }

    tlm_sync_enum nb_fw(tlm_generic_payload& gp, tlm_phase& ph, sc_time& delay) {
        if (ph == BEGIN_REQ) {
            wait_q.push(&gp);            // 进仲裁队列
            chan_free_ev.notify(delay);  // 唤醒仲裁器
            return TLM_ACCEPTED;
        }
        return TLM_ACCEPTED;
    }

    // round-robin / FIFO 仲裁：通道一次只服务一个请求 -> 制造 contention
    void arbiter() {
        while (true) {
            if (wait_q.empty() || channel_busy) { wait(chan_free_ev); continue; }
            channel_busy = true;
            auto* gp = wait_q.front(); wait_q.pop();
            tlm_phase ph = BEGIN_REQ; sc_time d = SC_ZERO_TIME;
            isock->nb_transport_fw(*gp, ph, d);   // 转发给 Memory
            // 通道占用直到该事务完成（简化：等 BEGIN_RESP 回来在 nb_bw 里释放）
            wait(chan_free_ev);
        }
    }

    tlm_sync_enum nb_bw(tlm_generic_payload& gp, tlm_phase& ph, sc_time& delay) {
        if (ph == BEGIN_RESP) {
            channel_busy = false;          // 释放通道，可服务下一个
            chan_free_ev.notify(delay);
            tlm_phase p = ph; sc_time d = delay;
            return tsock->nb_transport_bw(gp, p, d);  // 透传回 DMA
        }
        return TLM_ACCEPTED;
    }
};
```

> **要点**：Interconnect 的核心价值在于仲裁、排队、通道独占导致有效带宽被分摊，这是总线架构中的常见设计问题。博客里可重点展开这块。

### 1.4 dma_engine.h —— 数据搬运（AT initiator）

```cpp
#pragma once
#include "common.h"

struct DmaEngine : sc_module {
    tlm_utils::simple_initiator_socket<DmaEngine> isock;
    NpuConfig cfg;
    uint32_t outstanding = 0;
    sc_event done_ev;        // 一次搬运完成通知 PE/Driver

    SC_HAS_PROCESS(DmaEngine);
    DmaEngine(sc_module_name n, NpuConfig c)
      : sc_module(n), isock("isock"), cfg(c) {
        isock.register_nb_transport_bw(this, &DmaEngine::nb_bw);
    }

    // 发起一次 tile 搬运（被 Driver / PE 调用）
    void fetch(uint64_t addr, uint32_t bytes, TileExtension::Kind kind) {
        auto* gp = new tlm_generic_payload();
        gp->set_command(TLM_READ_COMMAND);
        gp->set_address(addr);
        gp->set_data_length(bytes);
        auto* ext = new TileExtension(); ext->kind = kind;
        gp->set_extension(ext);
        tlm_phase ph = BEGIN_REQ; sc_time d = SC_ZERO_TIME;
        outstanding++;
        isock->nb_transport_fw(*gp, ph, d);
    }

    tlm_sync_enum nb_bw(tlm_generic_payload& gp, tlm_phase& ph, sc_time& delay) {
        if (ph == BEGIN_RESP) {
            outstanding--;
            done_ev.notify(delay);               // 通知"这块数据到了"
            tlm_phase p = END_RESP; sc_time d = delay;
            isock->nb_transport_fw(gp, p, d);     // 回 END_RESP 收尾
            return TLM_COMPLETED;
        }
        return TLM_ACCEPTED;
    }
};
```

### 1.5 onchip_buffer.h —— 片上 SRAM

```cpp
#pragma once
#include "common.h"

// 容量 + 带宽双约束的 timing model（不建替换策略）
struct OnchipBuffer : sc_module {
    NpuConfig cfg;
    uint32_t used_bytes = 0;
    OnchipBuffer(sc_module_name n, NpuConfig c) : sc_module(n), cfg(c) {}

    bool can_hold(uint32_t bytes) const {
        return used_bytes + bytes <= cfg.buffer_kb * 1024;
    }
    void allocate(uint32_t bytes) { used_bytes += bytes; }
    void release(uint32_t bytes)  { used_bytes -= bytes; }

    // 读/写一块数据占用的时间 = bytes / buffer 带宽
    sc_time access_time(uint32_t bytes) const {
        return cycles(uint64_t(std::ceil(bytes / cfg.buf_bw_Bpc)));
    }
};
```

### 1.6 pe_array.h —— 脉动阵列（核心 timing 单元）

```cpp
#pragma once
#include "common.h"

struct PeArray : sc_module {
    NpuConfig cfg;
    // 统计
    uint64_t busy_cycles = 0;
    uint64_t total_cycles = 0;
    sc_event compute_done_ev;

    SC_HAS_PROCESS(PeArray);
    PeArray(sc_module_name n, NpuConfig c) : sc_module(n), cfg(c) {}

    // 计算一个 tile：消耗 compute_cycles（公式见第 2 部分），期间标记 busy
    void compute_tile(uint32_t tile_rows, uint32_t tile_cols, uint32_t tile_k) {
        uint64_t cyc = tile_compute_cycles(tile_rows, tile_cols, tile_k);
        busy_cycles += cyc;
        // 注意：这里只 wait，不做真实乘加 —— 性能模型的精髓
        wait(cycles(cyc));
        compute_done_ev.notify(SC_ZERO_TIME);
    }

    // === 关键公式，详见第 2 部分 ===
    uint64_t tile_compute_cycles(uint32_t Tr, uint32_t Tc, uint32_t Tk) {
        uint32_t N = cfg.array_n;
        uint64_t fill  = 2 * N;                 // 流水填充
        uint64_t steady = uint64_t(Tk) * std::ceil(double(Tc)/N); // 稳态
        uint64_t drain = N;                     // 排空
        // Tr 维度的 tile 复用映射在 Driver 层循环，这里建单次阵列计算
        return fill + steady + drain;
    }

    double utilization() const {
        return total_cycles ? double(busy_cycles)/total_cycles : 0.0;
    }
};
```

### 1.7 workload_driver.h —— tiling 与调度

```cpp
#pragma once
#include "common.h"
#include "pe_array.h"
#include "dma_engine.h"
#include "onchip_buffer.h"

struct WorkloadDriver : sc_module {
    NpuConfig cfg;
    GemmTask  task;
    PeArray*      pe;
    DmaEngine*    dma;
    OnchipBuffer* buf;

    SC_HAS_PROCESS(WorkloadDriver);
    WorkloadDriver(sc_module_name n, NpuConfig c, GemmTask t,
                   PeArray* p, DmaEngine* d, OnchipBuffer* b)
      : sc_module(n), cfg(c), task(t), pe(p), dma(d), buf(b) {
        SC_THREAD(run);
    }

    void run() {
        uint32_t N = cfg.array_n;
        // tiling：把 M×N×K 切成 N×N 的 tile（简化为方形 tile）
        uint32_t tiles_m = std::ceil(double(task.M)/N);
        uint32_t tiles_n = std::ceil(double(task.N)/N);
        uint32_t tiles_k = std::ceil(double(task.K)/N);

        uint64_t start = sc_time_stamp().value();

        for (uint32_t i = 0; i < tiles_m; ++i)
          for (uint32_t j = 0; j < tiles_n; ++j)
            for (uint32_t k = 0; k < tiles_k; ++k) {
                uint32_t bytes = N * N * cfg.data_bytes;
                if (cfg.double_buffer) {
                    // 预取下一 tile 的同时算当前 tile -> overlap
                    dma->fetch(/*addr*/0, bytes, TileExtension::WEIGHT);
                    pe->compute_tile(N, N, N);       // 与 DMA 重叠
                    wait(dma->done_ev);
                } else {
                    dma->fetch(0, bytes, TileExtension::WEIGHT);
                    wait(dma->done_ev);              // 串行：先等数据
                    pe->compute_tile(N, N, N);
                }
            }

        uint64_t end = sc_time_stamp().value();
        pe->total_cycles = (end - start); // 简化：用时间戳差近似
        sc_stop();
    }
};
```

### 1.8 perf_monitor.h + main —— 统计与组装

```cpp
// perf_monitor.h：收集并输出 CSV
struct PerfMonitor {
    static void report(const NpuConfig& cfg, const GemmTask& t,
                       const PeArray& pe, double sim_time_s) {
        double flops = 2.0 * t.M * t.N * t.K;        // GEMM 总运算数
        double gops  = flops / sim_time_s / 1e9;
        double ai    = flops / bytes_moved(cfg, t);  // arithmetic intensity
        std::cout << "M,K,N,array_n,buffer_kb,double_buf,sim_us,GOPS,util,AI\n";
        std::cout << t.M<<","<<t.K<<","<<t.N<<","<<cfg.array_n<<","
                  << cfg.buffer_kb<<","<<cfg.double_buffer<<","
                  << sim_time_s*1e6<<","<<gops<<","
                  << pe.utilization()<<","<<ai<<"\n";
    }
    static double bytes_moved(const NpuConfig& c, const GemmTask& t) {
        // 估算 HBM 实际搬运字节（受 buffer reuse 影响）—— 详见第 2 部分
        return double(t.M)*t.K + double(t.K)*t.N + double(t.M)*t.N;
    }
};
```

```cpp
// main.cpp：组装拓扑
int sc_main(int argc, char* argv[]) {
    NpuConfig cfg;            // 可从 argv 覆盖 array_n / buffer_kb / double_buffer
    GemmTask  task{512, 512, 512};

    Memory       mem("mem", cfg);
    Interconnect ic("ic", cfg);          // 加分模块；MVP 可先直连
    DmaEngine    dma("dma", cfg);
    OnchipBuffer buf("buf", cfg);
    PeArray      pe("pe", cfg);
    WorkloadDriver drv("drv", cfg, task, &pe, &dma, &buf);

    // 绑定：DMA -> Interconnect -> Memory
    dma.isock.bind(ic.tsock);
    ic.isock.bind(mem.tsock);

    sc_time t0 = sc_time_stamp();
    sc_start();
    double sim_s = sc_time_stamp().to_seconds();
    PerfMonitor::report(cfg, task, pe, sim_s);
    return 0;
}
```

> **MVP 简化建议**：第一版可让 `dma.isock` 直接 `bind(mem.tsock)`，跳过 Interconnect，先验证 PE/DMA overlap 正确，再插入仲裁器做 contention 实验。

---

## 第 2 部分 · 脉动阵列 Timing 公式精确推导

> 这一节给 PE 模型提供**清晰的数学依据**。每个 cycle 数都能从阵列结构推导出来，这是模型可信度的基础。

### 2.1 脉动阵列工作模型

设 N×N 的 weight-stationary 阵列：权重预加载到 PE，激活从左边流入，逐 cycle 向右传播并做 MAC，部分和向下累加。

**单个 N×N tile（权重 N×N，激活 N×N，输出 N×N）的时序分三段：**

```
   cycle →
   |<- fill ->|<----- steady ----->|<- drain ->|
   填充流水     稳态满负荷产出          排空流水
```

**(1) 填充延迟 fill**
激活从第一列流到最后一列、部分和从第一行累加到最后一行，对角线传播需要：
```
fill = 2N - 2  ≈ 2N   cycle
```
（第一个输出在 ~2N cycle 后出现；保守取 2N。）

**(2) 稳态 steady**
权重固定后，每 cycle 喂入一列激活向量。处理 Tk 行激活（K 维度 tile 深度 = Tk）、输出 Tc 列时：
```
steady = Tk · ceil(Tc / N)   cycle
```
对方形 tile（Tk = Tc = N）：`steady = N`。

**(3) 排空 drain**
最后一个激活流过整个阵列：
```
drain = N - 1 ≈ N   cycle
```

**单 tile 总周期：**
```
T_tile = fill + steady + drain
       ≈ 2N + Tk·ceil(Tc/N) + N
       = 3N + Tk·ceil(Tc/N)
```
方形 tile 时 `T_tile ≈ 4N`。

### 2.2 整个 GEMM 的 tile 数与理想计算周期

GEMM 维度 M×K×N，按 N×N tile 切分：
```
tiles_m = ceil(M/N)
tiles_n = ceil(N_dim/N)
tiles_k = ceil(K/N)
total_tiles = tiles_m · tiles_n · tiles_k
```

**理想计算周期（compute-bound 下界，假设数据永远就绪）：**
```
T_compute = total_tiles · T_tile
```

当 tile 很多时，fill/drain 的固定开销被摊薄，每个 tile 趋近 steady 的 N cycle，阵列利用率趋近：
```
util_compute = steady / T_tile = N / (4N) ... 实际随 tile 复用率上升
```
> 这解释了一个重要现象：**小 workload 喂不饱大阵列**，fill/drain 占比高，利用率低 —— 正是你"阵列规模扫描"实验要复现的洞察。

### 2.3 访存下界（memory-bound 下界）

**实际从 HBM 搬运的字节数**（关键：受 buffer 容量决定的 data reuse 影响）。

理想情况（无限 buffer，每个数据只搬一次）：
```
Bytes_min = (M·K + K·N + M·N) · data_bytes
```

有限 buffer 时，tile 需要反复加载，搬运量放大。设 tile 大小 N×N，weight-stationary 下激活被复用 tiles_m 次、权重被复用 tiles_n 次的程度取决于循环顺序与 buffer 能否驻留：
```
Bytes_actual ≈ Bytes_min · reuse_penalty(buffer_kb)
```
`reuse_penalty` 随 buffer 增大而趋近 1 —— 这是你"buffer 敏感性"实验的核心曲线。

**访存时间下界：**
```
T_memory = Bytes_actual / HBM_bandwidth
```

### 2.4 Roofline：把两个下界合起来

**总执行时间 = compute 与 memory 的较大者**（理想 overlap 下）：
```
T_total ≈ max( T_compute , T_memory )
```

- 当 `T_compute > T_memory` → **compute-bound**，瓶颈在阵列吞吐，利用率高。
- 当 `T_memory > T_compute` → **memory-bound**，瓶颈在 HBM 带宽，阵列空转等数据。

**Arithmetic Intensity（横轴）：**
```
AI = FLOPs / Bytes_actual = (2·M·K·N) / Bytes_actual   [FLOP/Byte]
```

**Roofline 屋顶：**
```
Attainable_perf = min( Peak_compute , AI · HBM_bandwidth )

Peak_compute = N·N · 2 · CLK_FREQ   [FLOP/s]   (N² 个 MAC，每 MAC 2 FLOP)
拐点 AI*     = Peak_compute / HBM_bandwidth
```
`AI < AI*` 落在带宽屋檐（斜线），`AI > AI*` 落在算力屋顶（水平线）。

### 2.5 Double buffering 对总时间的影响

- **无 overlap（串行）**：`T_total = T_compute + T_memory`
- **理想 overlap（double buffer）**：`T_total = max(T_compute, T_memory)`

二者之比就是预取收益。你的实验只要开/关 `cfg.double_buffer`，对比这两条公式与仿真值，三者吻合即验证模型正确。

### 2.6 验证用的解析速查表（直接拿去对账）

| 量 | 公式 |
|----|------|
| 单 tile 周期 | `T_tile = 3N + Tk·ceil(Tc/N)` |
| tile 总数 | `ceil(M/N)·ceil(N_dim/N)·ceil(K/N)` |
| 计算下界 | `T_compute = total_tiles · T_tile` |
| 访存下界 | `T_memory = Bytes_actual / BW_hbm` |
| 总时间(overlap) | `max(T_compute, T_memory)` |
| 总时间(串行) | `T_compute + T_memory` |
| 算术强度 | `AI = 2MKN / Bytes_actual` |
| 峰值算力 | `2N²·freq` |
| roofline 拐点 | `AI* = 2N²·freq / BW_hbm` |
| 阵列利用率 | `busy_cycles / total_cycles` |

> **validation 方法**：对每个实验点，用上表手算一遍 → 和 SystemC 仿真输出对比 → 记录相对误差。误差来源（fill/drain 近似、tiling 边界、PEQ 量化）写进博客的误差分析。

---

## 第 3 部分 · 博客大纲与关键图清单

> 博客是项目的对外输出，结构本身应展示 performance modeling 的完整思维链：动机 → 抽象取舍 → 建模 → 实验洞察 → 验证 → 边界。

### 3.1 博客结构

**标题建议**：《用 SystemC TLM-AT 给脉动阵列 NPU 建一个性能模型：从抽象选择到 Roofline 验证》

1. **动机（为什么建这个）**
   - 性能建模在 NPU/GPU 架构设计中的角色：流片前预测、辅助架构决策。
   - 点出功能建模 vs 性能建模的根本区别（"对不对" → "多快"）。
   - 简述从功能建模到性能建模的能力迁移路径。

2. **抽象层次的选择（体现成熟度，重点段落）**
   - 为什么选 cycle-approximate 而非 cycle-accurate 或纯解析。
   - PE 阵列为什么只建 timing 不做真实 MAC。
   - HBM 为什么只建带宽+延迟、不建 DRAM 行时序。
   - **明确写出做了哪些简化假设、为什么这样取舍合理** —— 这是架构判断力的核心体现。

3. **架构与模块划分**
   - 放整体框图（图 1）。
   - 逐模块说职责：PE Array / Buffer / DMA / Interconnect / Memory / Driver。

4. **TLM-AT 建模细节（技术含金量核心）**
   - AT vs LT 的取舍：哪些路径用 AT、哪些用 LT，为什么。
   - 4-phase 协议（BEGIN_REQ/END_REQ/BEGIN_RESP/END_RESP）在 DMA↔IC↔Mem 路径上怎么流转（图 2 时序图）。
   - **Contention 怎么建**：仲裁器、排队、通道独占 —— 总线架构中的常见问题。
   - temporal decoupling / quantum 对精度的影响。

5. **实验与洞察（配图，决定说服力）**
   - 五组实验（见 3.2）。每组：实验设计 → 图 → 洞察一句话。

6. **Validation 与误差分析（最稀缺，务必写满）**
   - 解析模型交叉验证：仿真 vs 手算，误差表。
   - Sanity check：极端 case 行为符合物理直觉。
   - 误差来源逐条剖析、模型适用边界。

7. **局限与展望**
   - 当前简化的代价、下一步可加什么（多 dataflow、量化、稀疏、多核 NPU）。
   - 收尾呼应：这套方法论怎么迁移到真实 NPU/GPU 架构评估。

### 3.2 必画的关键图清单

| # | 图 | 类型 | 展示什么 | 对应实验 |
|---|----|----|---------|---------|
| 1 | 架构框图 | 框图 | 模块与 socket 连接 | — |
| 2 | AT 4-phase 时序图 | 时序图 | 一次 DMA 事务的相位流转 | — |
| 3 | **Roofline 图** | 散点+折线 | 不同 GEMM 形状落点，验证 compute/memory-bound | 实验1 |
| 4 | Buffer 敏感性曲线 | 折线 | buffer 容量 ↑ → HBM 流量 ↓、利用率 ↑（收益递减） | 实验2 |
| 5 | Double buffering 对比 | 柱状 | overlap 开/关的总时间与利用率差异 | 实验3 |
| 6 | Contention 退化曲线 | 折线 | 并发 DMA channel ↑ → 有效带宽 ↓、延迟 ↑ | 实验4 |
| 7 | 阵列规模扫描 | 双轴折线 | 8/16/32 阵列：峰值算力 ↑ vs 利用率 ↓ | 实验5 |
| 8 | **Validation 对账表/图** | 表或散点 | 仿真 vs 解析，相对误差 | 全部 |

> 图 3（Roofline）和图 8（Validation）是两张关键图，务必做精。Roofline 证明模型对硬件物理规律的理解，Validation 证明模型的可信度 —— 这正是性能建模工作的两个核心支柱。

### 3.3 五组实验速查

| 实验 | 扫描变量 | 固定量 | 预期洞察 |
|------|---------|--------|---------|
| 1 Roofline 定位 | GEMM 形状 (改 M/K/N 改变 AI) | 硬件配置 | 低 AI 卡带宽、高 AI 卡算力 |
| 2 Buffer 敏感性 | buffer_kb | workload | reuse 提升降低 HBM 需求，收益递减 |
| 3 Double buffering | double_buffer 开/关 | 其余 | 量化预取收益 = 串行/overlap 之比 |
| 4 Contention | 并发 DMA channel 数 | 其余 | 有效带宽分摊、延迟上升 |
| 5 阵列规模 | array_n (8/16/32) | workload | 大阵列峰值高但小负载利用率低 |

---

## 附录 · 技术深聊清单（自检理解深度）

做完项目后，确保能清晰回答以下问题——这反映了对模型各层面的理解深度：

1. 为什么用 AT 而不是 LT？哪条路径用了 AT，为什么其余的没用？
2. 脉动阵列 cycle 公式怎么推的？fill/drain 为什么是 2N/N？
3. contention 是怎么在模型里产生的？仲裁策略换成 priority 会怎样？
4. 怎么验证模型是对的？误差多少？误差从哪来？
5. 模型在什么假设下会失真？要提精度你会先加哪一块？
6. temporal decoupling 的 quantum 设大了会怎样？精度和仿真速度怎么权衡？
7. 如果给真实 NPU 加 weight 量化 / 稀疏，性能模型要改哪里？

这 7 个问题覆盖了模型设计的核心决策点，能讲清楚就意味着对整套方法论有了系统掌握。
