# npu-perf-model

> ## 🚧 Work in Progress
>
> A performance modeling framework for NPU architecture exploration.
>
> 当前进度：**MVP-1（LT 数据流）+ MVP-2（PE Array + double buffering）+ MVP-3（DMA↔Memory 四相 AT）+ MVP-4（Interconnect/Arbiter + contention）已完成**。

用 SystemC TLM2.0（DMA → Interconnect → MemoryController → HBM 全链路为四相 AT；其余模块为 timing abstraction）搭建的 weight-stationary 脉动阵列 NPU **性能模型**：输入 GEMM workload，输出吞吐 / 延迟 / 利用率预测，并支持 roofline 分析、配置敏感性研究与 validation。

## 技术栈

C++17 + SystemC 2.3.x + TLM2.0（Accellera 开源版）+ CMake

## 快速开始

```bash
# 1. 一键下载并编译 SystemC(含 TLM2.0)到 third_party/systemc(仅首次需要)
scripts/setup_systemc.sh            # 强制重编: scripts/setup_systemc.sh --force

# 2. 构建主工程与测试
cmake -S . -B build && cmake --build build -j

# 3. 跑 sanity 测试与仿真
ctest --test-dir build --output-on-failure
./build/npu_sim 512 512 512 16 256  # 用法: npu_sim [M K N] [array_n] [buffer_kb]
```

> SystemC 库与 `build/` 均不入库,靠上面脚本/CMake 现场生成。

## 抽象层次

cycle-approximate（近似周期级）：不做 cycle-accurate RTL，也不做纯解析。计算单元只建 timing，不做真实 MAC。

## 目录结构

```
npu-perf-model/
├── CMakeLists.txt
├── README.md
├── docs/
│   └── design/
│       ├── NPU_Perf_Model_DevDoc.md   // 开发文档：背景知识、模块设计、公式推导、博客大纲
│       ├── MVP1_dataflow.md           // MVP-1 数据流程图
│       ├── MVP3_at_dataflow.md        // MVP-3 四相 AT 数据流
│       ├── MVP4-Class-Diagram-SystemC-Connection.md // MVP-4 类图与 socket 连接
│       └── MVP4-Detailed-Design-Specification.md    // MVP-4 详细设计规范
├── include/
│   ├── common.h           // 全局类型、NpuConfig、cycle<->time 换算、TileExtension
│   ├── request.h          // MemoryRequest：请求状态机 + issue/grant/finish 时间戳
│   ├── arbiter.h          // 仲裁策略（FIFO / Round-Robin / Priority）
│   ├── pe_array.h
│   ├── onchip_buffer.h
│   ├── dma_engine.h       // AT initiator（四相握手 + outstanding 窗口）
│   ├── interconnect.h     // 队列 + 仲裁 + 背压 + 转发带宽串行化
│   ├── memory_controller.h // HBM 数据通道带宽串行化 bridge
│   ├── hbm.h              // HBM 固定延迟 target
│   ├── npu_system.h       // 顶层组装与 socket 绑定
│   ├── workload_driver.h
│   └── perf_monitor.h
├── src/
│   └── (对应 .cpp，或 header-only)
├── tests/
│   └── sanity_tests.cpp
└── scripts/
    └── plot_results.py   // 读 CSV 出 roofline / 敏感性曲线
```

## 核心模块

- **PE Array**：脉动阵列，weight-stationary dataflow，建 fill/steady/drain 三阶段 timing。
- **Onchip Buffer**：容量 + 带宽双约束的片上 SRAM timing 模型。
- **DMA Engine**：AT initiator，四相握手 + outstanding 窗口，负责数据搬运。
- **Interconnect**：队列 + 仲裁 + 背压 + 转发带宽串行化，建模多 DMA 竞争 contention。
- **Memory Controller**：HBM 数据通道带宽串行化 bridge（插在 Interconnect 与 HBM 之间）。
- **HBM**：固定访问延迟 target（延迟可被多笔 outstanding 请求重叠隐藏）。
- **NpuSystem**：顶层模块，负责实例化与 socket 绑定。
- **Workload Driver**：GEMM tiling 与调度，支持 double buffering overlap。
- **Perf Monitor**：统计吞吐 / 利用率 / arithmetic intensity 并输出 CSV。

## 开发计划

| 阶段 | 内容 | 状态 |
|------|------|------|
| MVP-1 | common + Memory + Buffer + 单 DMA（LT 先跑通数据流） | ✅ 已完成 |
| MVP-2 | PE Array timing + double buffering overlap，输出吞吐/利用率 | ✅ 已完成 |
| MVP-3 | DMA↔Memory 路径升级为 AT（4 phases + PEQ） | ✅ 已完成 |
| MVP-4 | Interconnect/Arbiter + contention 实验 | ✅ 已完成 |
| MVP-5 | 解析模型交叉验证 + 敏感性扫描脚本 + 博客 | ⬜ 未开始 |

先用 LT 把功能跑通，再把关键路径换 AT。

**MVP-2 说明**：PE Array 建 fill/steady/drain 三阶段 timing；double buffering 提供两种实现——
解析重叠（单线程 `max(load, compute)`）与双 `SC_THREAD` 真并发（loader/compute + ping-pong 槽 +
`sc_event` 生产者-消费者同步，可跨 output tile 预取）。通过 `--serial` 关闭双缓冲作对照。

**MVP-4 说明**：访存路径升级为 N 个 DMA → Interconnect（队列+仲裁+背压）→ MemoryController
（带宽串行化）→ HBM（固定延迟）。新增命令行参数
`--dma N --arbiter fifo|rr|priority --noc-latency N --queue-depth N --interconnect-bw G`；
有效带宽 = min(interconnect_bw, hbm_bw)；队列满时背压（暂不回 END_REQ，腾槽后 promote）。

### MVP-4 代码导读（从哪里开始看）

推荐按"一条访存请求的旅程"顺序读：

1. **先跑起来建立直觉**
   ```bash
   ./build/npu_sim --dma 4 --arbiter rr --noc-latency 2   # 看 grants/source 与 contention
   ./build/sanity_tests                                    # 42 项断言，每项对应一个设计点
   ```
2. **读两份设计文档建立全局图（约 10 分钟）**
   - `docs/design/MVP4-Detailed-Design-Specification.md`：§5 互连 / §6 仲裁 / §7 MC / §8 时序公式 / §9 背压
   - `docs/design/MVP4-Class-Diagram-SystemC-Connection.md`：模块类图与 socket 连接关系
3. **按数据流顺序读代码**

   | 顺序 | 文件 | 看什么 |
   |---|---|---|
   | 1 | `src/main.cpp` | 入口：参数解析 → NpuSystem → 报告 |
   | 2 | `include/npu_system.h` | 拓扑组装：N 个 DMA → IC → MC → HBM 的 socket 绑定 |
   | 3 | `include/common.h` | NpuConfig 全部可调参数、TileExtension、cycle 换算 |
   | 4 | `include/request.h` | MemoryRequest 状态机与 issue/grant/finish 三时间戳 |
   | 5 | `include/arbiter.h` | 三种仲裁策略（纯 C++ 类，无独立时序） |
   | 6 | `src/interconnect.cpp` | ★核心：forward_loop 仲裁+带宽串行化、背压 promote |
   | 7 | `src/memory_controller.cpp` | 数据通道带宽串行化（next_data_free_ + PEQ） |
   | 8 | `src/hbm.cpp` | 固定延迟 target：PEQ 调度 END_REQ/BEGIN_RESP |
   | 9 | `src/dma_engine.cpp` | AT initiator：outstanding 窗口、Transfer 生命周期 |
   | 10 | `src/workload_driver.cpp` | 调度层：串行 vs 双缓冲（loader/compute 生产者-消费者） |
   | 11 | `src/perf_monitor.cpp` | 指标汇总与 CSV 输出 |

4. **一条 read 请求的完整四相时序（贯穿主线）**
   ```
   DMA BEGIN_REQ → IC 入队并回 END_REQ → forward_loop 仲裁+等互连带宽
     → 转发(noc_latency) → MC 转发 → Hbm PEQ: END_REQ@arrival / BEGIN_RESP@ready
     → MC 串行带宽至 data_done → IC 记账并回 BEGIN_RESP → DMA 回 END_RESP 逐级闭合
   ```

## 实验

1. **Roofline 定位**：改变 GEMM 形状（M/K/N）观察 compute-bound / memory-bound 分布。
2. **Buffer 敏感性**：扫描 buffer 容量，观察 HBM 流量与利用率变化。
3. **Double buffering**：对比预取开/关的总时间与利用率差异。
4. **Contention**：扫描并发 DMA channel 数，观察有效带宽与延迟退化。
5. **阵列规模扫描**：对比 8/16/32 阵列的峰值算力与利用率。

## 文档

- 完整开发文档（前置背景知识、模块代码骨架、timing 公式推导、博客大纲与实验设计）：[docs/design/NPU_Perf_Model_DevDoc.md](docs/design/NPU_Perf_Model_DevDoc.md)
- Phase-1 (MVP-1) 数据流程图（模块拓扑、tiling 循环、LT 时序、时间对账）：[docs/design/MVP1_dataflow.md](docs/design/MVP1_dataflow.md)
- Phase-3 (MVP-3) AT 数据框架图与四相协议原理图：[docs/design/MVP3_at_dataflow.md](docs/design/MVP3_at_dataflow.md)
- Phase-4 (MVP-4) 类图与 SystemC 连接关系：[docs/design/MVP4-Class-Diagram-SystemC-Connection.md](docs/design/MVP4-Class-Diagram-SystemC-Connection.md)
- Phase-4 (MVP-4) 详细设计规范（互连/仲裁/MC/背压/时序模型）：[docs/design/MVP4-Detailed-Design-Specification.md](docs/design/MVP4-Detailed-Design-Specification.md)
