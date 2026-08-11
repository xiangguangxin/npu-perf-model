# SystemC 与 QEMU 联合仿真方案

## 为什么要把两者接起来

- **QEMU**:动态二进制翻译(TCG),跑 CPU + 标准外设**极快**,能跑真实固件/OS/驱动软件,但不是 cycle-accurate,时序基本没有意义。
- **SystemC/TLM**:建自定义 IP(比如 NPU 加速器、自定义总线/DMA)的 timing,精度可控(LT/AT),但速度远不如 QEMU,而且默认不跑真实指令流。

两者结合的典型动机:**CPU + 标准外设交给 QEMU 跑(快、功能真实),自定义加速器交给 SystemC 建 timing(准)**,组成一个混合虚拟平台——软件(驱动/固件)真实运行,通过真实的 MMIO/DMA 路径去驱动 timing 精确的自定义 IP 模型。这正是 Xilinx/AMD Versal 一类"PS(ARM 核,QEMU 建)+ PL(自定义加速器,SystemC 建)"虚拟平台的做法,和"用真实驱动软件去 kick off NPU 模型"这个需求高度对应。

## 两种主流架构

### A. 同进程集成(in-process,共享地址空间)

QEMU 编译成 library 形式,和 SystemC kernel 跑在同一个进程里(可能不同 host 线程:QEMU 的 vCPU 线程 + SystemC 的单线程 event-driven kernel)。桥接层把 QEMU 侧的 CPU 包装成一个带 TLM socket 的 SystemC module:

```
┌─────────────────────────── 同一个进程 ───────────────────────────┐
│                                                                    │
│   QEMU (TCG, vCPU 线程)              SystemC kernel(event-driven) │
│   ┌───────────────┐   MMIO 拦截    ┌─────────────────────────┐    │
│   │  guest 软件    │───────────────►│ TLM bridge (initiator)  │    │
│   │  (驱动/固件)   │◄───────────────│  → NPU model / bus      │    │
│   └───────────────┘   中断回注     └─────────────────────────┘    │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

- SystemC 建模的寄存器地址区间注册进 QEMU 的 `MemoryRegion` 树,读写回调转成 `tlm_generic_payload` + `b_transport` 调用;反过来 SystemC 里 DMA-capable 的设备要访问 guest RAM,就转成 QEMU 的地址空间读写。
- 中断:SystemC 设备产生的中断映射成 QEMU 对应 vCPU 的 IRQ line(GPIO 风格的 signal port)。
- 代表实现:**GreenSocs libqbox**——把 QEMU CPU + 内存子系例包装成带 TLM socket 的 SystemC module,是目前最活跃的"QEMU 作为 SystemC/TLM 组件"方案。
- 优点:通信是函数调用/共享内存,延迟低、调试相对简单。
- 缺点:两种截然不同的并发模型(QEMU 多线程 + SystemC 单线程 event loop)耦合在一个进程里;GPL 的 QEMU 代码和闭源 SystemC IP 混进一个二进制也可能有license 顾虑;不利于扩展到多机分布式仿真。

### B. 跨进程集成(out-of-process,remote-port 协议)

QEMU 和 SystemC 各自作为独立 OS 进程运行,通过一个轻量协议通信——控制面用 Unix domain socket 传"事务描述"(地址/长度/命令/时间戳),数据面用共享内存传实际读写数据(避免大块 DMA 数据来回拷贝)。

```
┌────────────── 进程 A ──────────────┐        ┌────────────── 进程 B ──────────────┐
│ QEMU                                │ socket │ SystemC                             │
│  guest 软件 → MMIO 拦截 ────────────┼───────►│ remote-port-tlm adapter → NPU model  │
│  vCPU IRQ line  ◄───────────────────┼◄───────┤        (TLM socket)                 │
└──────────────────────────────────────┘ shm  └──────────────────────────────────────┘
```

- 代表实现:**GreenSocs remote-port**,最早为 Xilinx Zynq/Versal 虚拟平台设计——QEMU 跑 ARM 核 + 标准外设,SystemC 跑自定义 IP,两边进程隔离。
- 协议消息的字段基本对应 `tlm_generic_payload`(address/length/command/data/byte-enable),外加显式的"sync"报文携带时间戳,用于双方按 quantum 对齐仿真时间。
- 优点:进程隔离(GPL 代码和闭源 IP 分开、可分别构建/分发),可以跑在不同机器/核上做扩展,协议通用,以后换掉 QEMU 用别的快速模拟器也不用改 SystemC 侧模型。
- 缺点:多了一层 IPC 延迟,需要显式设计同步协议,调试要跨两个进程。

## 时间同步(核心难点)

### 为什么难

QEMU 和 SystemC 是两种完全不同的并发/时间模型:

- **QEMU**:vCPU 线程默认"能跑多快跑多快",指令执行速度取决于 host CPU 性能,和"虚拟时间"没有确定性对应关系——同一段 guest 代码,host 快就跑得快,不可重复。
- **SystemC**:严格的离散事件仿真,`sc_time_stamp()` 由内核按事件顺序精确推进,天然确定性。

要把两者拼到一起,必须先让 QEMU 的执行也变得"确定性、可按虚拟时间计费",否则 SystemC 侧根本没法知道"该在哪个时间点上跟 QEMU 对一次表"。

### icount:让 QEMU 变得有虚拟时间概念

QEMU 提供 `-icount shift=N`(或 `auto`)选项:

- 开启后,QEMU 不再"能跑多快跑多快",而是让每条(或每个 translation block 的)指令消耗一个固定的虚拟时钟节拍,`shift=N` 决定一条指令对应 `2^N` 个时钟周期。
- 这样 QEMU 内部就有了一条和 host 实际墙钟无关、纯粹按"执行了多少指令"推算出来的虚拟时间线,和 SystemC 的 `sc_time_stamp()` 是同一种"逻辑时间",可以对齐。
- 常配合 `align=on`(禁止 QEMU 因为等 host 墙钟而多跑或少跑)、`sleep=on/off`(QEMU 空闲时是否真的 sleep 而不是空转)一起用,进一步保证确定性。
- 代价:关掉了 QEMU"能跑多快跑多快"的默认优化,原始执行速度会下降,但这是能和外部仿真器做确定性联合仿真的前提——不开 icount,联调出来的时序毫无意义,只能做纯功能验证。

### 同步循环:跨模拟器版本的 quantum-based sync

思路和 TLM 内部的 temporal decoupling(`tlm_quantumkeeper`)完全一致,只是把"两个 SystemC 组件之间同步"换成了"QEMU 进程/线程 与 SystemC kernel 之间同步":

1. QEMU 侧在自己的 icount 时间线上连续执行一段指令(一个 quantum,比如几十/几百 us 的虚拟时间),期间产生的 MMIO/DMA 访问要么走本地缓存的地址范围直接处理,要么触发下面说的"强制同步点"。
2. quantum 用完(或触发强制同步)时,QEMU 侧把"这段时间消耗了多少虚拟时间"上报给 SystemC 侧。
3. SystemC kernel 用 `wait()` 把自己的仿真时间推进这么多,处理这段时间窗口内该发生的所有事件(定时器、其他组件的活动)。
4. SystemC 处理完,给 QEMU"放行"信号,QEMU 继续跑下一个 quantum。

**两类触发同步的场景**:

- **事务触发的同步(强制、及时)**:guest 侧访问了 SystemC 建模的地址区间(比如读写 NPU 寄存器、发起 DMA),这类访问必须等 SystemC 侧真实处理完并返回结果,天然是一个同步点,不能拖到 quantum 结束。
- **quantum 到期的同步(周期性、非强制)**:哪怕没有跨边界访问,也要定期同步,防止两边"虚拟时间差"越拉越大——这是为了让 SystemC 侧的定时器、异步产生的中断等能在合理延迟内被 guest 感知到。

### 中断的同步语义(容易被问到的坑)

中断是异步事件,可能在 QEMU 当前 quantum 执行到一半时,由 SystemC 侧的设备产生。常见处理方式:

- 简单实现:中断只在下一次同步点才真正"注入"给 vCPU——意味着中断延迟最大可能等于一个 quantum 的时长。
- 更精细的实现:允许中断**提前打断**当前 quantum,强制立即触发一次同步,把中断尽快注入,牺牲一些性能换取更真实的中断响应时序。
- 这也是 quantum 大小的权衡在联合仿真场景下的直接体现:quantum 越大,QEMU/SystemC 各自跑得越久、整体越快,但中断和跨部件的时序耦合就越不准。

### 确定性 vs 性能

- `icount` + 严格的 quantum 同步能做到**确定性、可重复**的联合仿真——同样的输入,不管 host 机器多快多慢,跑出来的仿真时间和行为完全一致,这对做性能建模、回归测试、bug 复现非常重要。
- 放松这些设置(比如不开 icount、或者同步靠 host 墙钟)能换取更高的原始执行速度,但仿真结果会依赖 host 性能、不可重复,通常只适合纯功能性的软件联调,不适合用来做 timing 分析或架构探索。

## 关键技术点(汇总)

1. **icount / 虚拟时间确定性**:见上文,是能做时间同步的前提。
2. **Quantum-based 时间同步**:见上文,是 QEMU-SystemC 联合仿真的核心机制。
3. **MMIO 拦截/转发**:SystemC 建模的地址区间要在两边协调好归属权,谁的地址谁负责响应。
4. **中断桥接**:SystemC 产生的中断要能设置到 QEMU 建模的 vCPU 的中断控制器输入上,并考虑好触发同步的时机。
5. **DMA 双向路由**:无论 QEMU 侧设备访问 SystemC 内存,还是 SystemC 侧设备(比如 NPU 的 DMA engine)访问 guest RAM,都要能双向转发 transaction。

## 和自己项目的结合思路

现在的 `workload_driver.h` 是直接在 C++ 里合成 GEMM tiling、调用 driver 接口,没有"真实软件栈通过 MMIO/DMA 描述符下发任务"这一层。如果要接 QEMU:

- **验证目标**:让 QEMU 跑一个真实(或简化)驱动固件,通过真实的寄存器配置 + DMA 描述符下发去 kick off NPU 模型,而不是 synthetic 的 tiling 循环——能验证真实软件交互延迟、真实的描述符处理路径,而不只是理想化的计算/搬运时序。
- **选型建议**:
  - 只是想快速验证"能不能跑通",选 **libqbox 同进程集成**更省事(不用自己维护同步协议)。
  - 如果希望把 NPU 性能模型做成独立 IP,给不同"host 虚拟平台"复用,或者要和其他团队分开维护代码库,选 **remote-port 跨进程**更合适。
