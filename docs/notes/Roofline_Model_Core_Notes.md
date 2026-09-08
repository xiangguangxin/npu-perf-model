# Roofline 模型核心学习笔记

## 1. Roofline 模型解决什么问题

Roofline 是一个性能上界模型，用来判断某个计算内核（kernel）在一台计算机上主要受到哪种资源限制：

- **计算受限（compute-bound）**：处理器的浮点计算能力成为瓶颈。
- **内存受限（memory-bound）**：片外内存向处理器提供数据的速度成为瓶颈。

它给出的不是程序一定能够达到的实际性能，而是程序在给定硬件和运算强度下的**性能上限**。

核心公式为：

$$
P_{\text{attainable}}
=
\min\left(P_{\text{peak}},\ B_{\text{memory}}\times I\right)
$$

其中：

- $P_{\text{attainable}}$：可达到的性能上限，单位通常为 GFLOP/s。
- $P_{\text{peak}}$：处理器峰值浮点性能。
- $B_{\text{memory}}$：可持续峰值内存带宽，单位通常为 GB/s。
- $I$：Operational Intensity（运算强度），单位为 FLOP/Byte。

---

## 2. Kernel、Working Set、Cache 和 Memory

### 2.1 Kernel

Kernel 在这里不是操作系统内核，而是程序中承担核心计算的一段代码，例如矩阵乘法、卷积或向量运算。

### 2.2 Working Set

Working set（工作集）是计算过程中需要访问的那部分数据。

原文：

> The working sets of the kernels we consider here do not fit fully in on-chip caches.

含义是：这些计算内核所需的数据不能全部装入片上 Cache，因此计算过程中必须持续访问 Cache 后面的主存系统。

### 2.3 Cache 与片外内存

- **on-chip caches**：片上缓存，通常包括 L1、L2，以及很多处理器中的 L3 Cache。
- **off-chip memory / DRAM**：片外主存，例如 DDR、GDDR 或 HBM。
- **memory system behind the caches**：位于 Cache 层次结构之后的主存系统，主要包括内存控制器、总线和 DRAM。

数据访问路径可以理解为：

```text
计算核心 → L1 Cache → L2 Cache → L3 Cache → 内存控制器 → DRAM
```

Cache 会过滤掉一部分内存访问：如果数据命中 Cache，就不需要访问 DRAM；只有未命中的访问才会形成 DRAM traffic。因此，经典 Roofline 中的 Operational Intensity 通常根据真正到达 DRAM 的字节数计算。

---

## 3. Roofline 图的两个坐标轴

### 3.1 X 轴：Operational Intensity

$$
I=\frac{\text{浮点运算次数}}{\text{访问 DRAM 的字节数}}
$$

单位是：

$$
\text{FLOP/Byte}
$$

它描述每从 DRAM 传输 1 Byte 数据，内核能够做多少次浮点运算。

- 运算强度低：数据利用率低，更容易受内存带宽限制。
- 运算强度高：同一份数据被重复计算或复用得更多，更容易接近计算峰值。

### 3.2 Y 轴：浮点性能

$$
P=\frac{\text{浮点运算次数}}{\text{运行时间}}
$$

单位通常是 GFLOP/s，即每秒十亿次浮点运算。

Roofline 屋顶表示理论或测得的性能上限；程序实际运行得到的性能点通常位于屋顶下方。

### 3.3 双对数坐标

经典 Roofline 使用 log-log scale（双对数坐标）。这使跨越多个数量级的数据能够清晰地显示，也使内存带宽线呈现为斜率为 1 的直线。

---

## 4. 水平线：峰值浮点计算性能

水平线表示整台计算机的峰值浮点计算性能：

$$
P_{\text{peak}}
=
\text{CPU 数量}
\times
\text{每颗 CPU 的核心数}
\times
\text{主频}
\times
\text{每核心每周期 FLOP 数}
$$

峰值性能不随运算强度增加而无限提高，因此在图中是一条水平线。程序性能不可能超过这一硬件计算上限。

### 4.1 双 Socket 示例

论文使用 2.2 GHz AMD Opteron X2 2214 双路系统：

- 2 个 Socket，即安装 2 颗物理 CPU。
- 每颗 CPU 有 2 个核心。
- 整机共有 4 个核心。
- 每核心主频为 2.2 GHz。
- 每核心每周期最多完成 2 次双精度浮点运算。

所以：

$$
P_{\text{peak}}
=2\times2\times2.2\times2
=17.6\ \text{GFLOP/s}
$$

### 4.2 为什么是每周期 2 次双精度运算

这不是“前半个周期算一次、后半个周期再算一次”，而是 SIMD 并行计算。

一个双精度浮点数占 64 bit，一条 128 bit SSE 向量指令可以同时处理两个双精度数：

```text
[a0, a1] + [b0, b1] → [a0+b0, a1+b1]
```

这条指令同时完成两个加法，因此计为 2 FLOP。如果处理器每周期能够启动一条这样的指令，吞吐率就是 2 FLOP/周期。

还要区分：

- **Latency（延迟）**：一条指令从开始到产生结果可能需要多个周期。
- **Throughput（吞吐率）**：流水线填满后，每周期可以启动或完成多少组计算。

现代处理器还需要考虑 AVX/AVX-512 向量宽度、每周期指令数以及 FMA。一条 FMA 对每个元素执行一次乘法和一次加法，通常计为 2 FLOP。

---

## 5. 斜线：峰值内存带宽所支撑的性能

对于给定运算强度，内存系统能够支撑的最大浮点性能为：

$$
P_{\text{memory}}=B_{\text{memory}}\times I
$$

从单位也可以验证：

$$
\frac{\text{Byte}}{\text{s}}
\times
\frac{\text{FLOP}}{\text{Byte}}
=
\frac{\text{FLOP}}{\text{s}}
$$

反过来：

$$
B_{\text{memory}}
=
\frac{P}{I}
=
\frac{\text{FLOP/s}}{\text{FLOP/Byte}}
=
\text{Byte/s}
$$

### 5.1 数值示例

假设可持续峰值内存带宽是：

$$
B_{\text{memory}}=15\ \text{GB/s}
$$

则：

| 运算强度 $I$ | 内存能够支撑的性能 $B\times I$ |
|---:|---:|
| 0.25 FLOP/Byte | 3.75 GFLOP/s |
| 0.5 FLOP/Byte | 7.5 GFLOP/s |
| 1 FLOP/Byte | 15 GFLOP/s |
| 2 FLOP/Byte | 30 GFLOP/s |

运算强度增加一倍，内存能够支撑的浮点性能也增加一倍，因此形成一条向右上方延伸的斜线。

在双对数坐标中：

$$
\log P=\log B_{\text{memory}}+\log I
$$

其斜率为 1，在横纵轴显示比例一致时看起来就是一条 45-degree angle（45 度角）的直线。45 度是双对数图中的视觉结果；在普通线性坐标中，直线的斜率实际是 $B_{\text{memory}}$，不一定呈 45 度。

---

## 6. 两条线为什么组成“屋顶”

性能同时受到两个上限约束：

1. 不能超过处理器的峰值计算性能。
2. 不能超过内存带宽在当前运算强度下能够支撑的性能。

因此必须取两者的较小值：

$$
P_{\text{attainable}}
=
\min(P_{\text{peak}},B_{\text{memory}}\times I)
$$

- 左侧斜线部分：内存带宽上限更低，因此是 memory-bound。
- 右侧水平部分：计算峰值上限更低，因此是 compute-bound。

水平线与斜线组合后形似屋顶，所以这个模型被称为 Roofline。

![AMD Opteron X2 的 Roofline 模型：黑色屋顶表示性能上限，两条紫色虚线分别对应内存受限和计算受限的内核](images/roofline_model_opteron_x2.svg)

*图：根据论文 Figure 1(a) 重绘的 AMD Opteron X2 Roofline 模型（双对数坐标，峰值算力 17.6 GFLOP/s，内存带宽 15 GB/s）。来源：Samuel Williams、Andrew Waterman、David Patterson，[Roofline: An Insightful Visual Performance Model for Floating-Point Programs and Multicore Architectures](../references/Roofline_Insightful_Visual_Performance_Model_2009.pdf)。*

图中的黑色斜线表示内存带宽所支撑的性能上限，水平线表示峰值浮点计算性能。两条紫色虚线展示了不同运算强度的内核：

- 左侧虚线（约 0.5 FLOP/Byte）先碰到斜线，因此属于 memory-bound。
- 右侧虚线（约 2 FLOP/Byte）先碰到水平线，因此属于 compute-bound。

虚线与屋顶的交点表示对应内核的性能上限；实际性能还需测量，通常位于交点下方。

### 6.1 Ridge Point：屋脊点

两条线的交点称为 ridge point：

$$
I_{\text{ridge}}
=
\frac{P_{\text{peak}}}{B_{\text{memory}}}
$$

对于论文中的系统：

$$
I_{\text{ridge}}
=
\frac{17.6}{15}
\approx1.17\ \text{FLOP/Byte}
$$

- $I<1.17$：理论上主要受内存带宽限制。
- $I>1.17$：理论上主要受计算峰值限制。

交点不是要求所有程序都必须达到的“最佳位置”，而是硬件瓶颈发生转换的位置。

---

## 7. 如何在图中定位一个 Kernel

对于一个给定的 kernel，可以根据它的 Operational Intensity 在 X 轴上确定位置，然后从该位置向上画一条竖线。

例如：

$$
I=1\ \text{FLOP/Byte}
$$

那么内核的 X 坐标固定为 1。它的实际性能可能是 5、10 或 14 GFLOP/s，所以性能点会位于 $X=1$ 的竖线上的某个位置。

原文中的：

> The performance of the kernel must lie somewhere along that line.

意思是：

> 该内核的性能点一定处于这条竖线上的某个位置。

这里不是说性能会沿着竖线移动，而是说运算强度已经确定了 X 坐标，但实际性能 Y 坐标还需要测量。竖线与 Roofline 的交点给出性能上限，实际性能点通常位于交点下方。

---

## 8. “柱子碰到屋顶”的比喻

可以把一个 kernel 的运算强度想象成从 X 轴向上延伸的柱子：

- 柱子碰到屋顶的倾斜部分：程序是 memory-bound。
- 柱子碰到屋顶的水平部分：程序是 compute-bound。

这个比喻强调的是：内核的 X 坐标由运算强度确定，向上寻找可达到的最高 Y 值时，最先碰到的屋顶部分决定了主要性能瓶颈。

---

## 9. 理论峰值、可达到上限与实际性能

三个概念需要区分：

| 概念 | 含义 | 获得方式 |
|---|---|---|
| 理论峰值性能 | 硬件在理想条件下的绝对计算上限 | 根据硬件规格计算 |
| Roofline 可达到上限 | 给定运算强度后，由计算峰值和内存带宽共同确定的上界 | 使用 $\min(P_{\text{peak}},B\times I)$ 计算 |
| 实际性能 | 程序真正运行得到的性能 | 根据 FLOP 数和运行时间测量 |

实际性能通常低于 Roofline 上限，因为还会受到以下因素影响：

- Cache miss 和访存延迟；
- 指令依赖和流水线停顿；
- SIMD 利用不足；
- 分支预测错误；
- 并行负载不均衡；
- 软件调度、循环结构和编译优化不足。

峰值内存带宽也应尽量使用 STREAM 或优化微基准测得的**可持续 DRAM 带宽**，而不是只采用 DRAM 引脚的理论带宽。

---

## 10. 重要英文术语

| 英文 | 中文含义 |
|---|---|
| kernel | 计算内核、核心计算代码 |
| working set | 工作集 |
| on-chip cache | 片上缓存 |
| off-chip memory | 片外内存 |
| memory system behind the caches | Cache 后面的主存系统 |
| operational intensity | 运算强度 |
| peak floating-point performance | 峰值浮点性能 |
| attainable performance | 可达到的性能上限 |
| peak memory bandwidth | 峰值内存带宽 |
| sustainable DRAM bandwidth | 可持续 DRAM 带宽 |
| horizontal line | 水平线 |
| diagonal / slanted line | 斜线、倾斜线 |
| angle | 角、角度 |
| a 45-degree angle | 一个 45 度角 |
| compute-bound | 计算受限 |
| memory-bound | 内存受限 |
| upper bound | 上界、上限 |
| dual-socket system | 双路系统、双 CPU 插槽系统 |
| somewhere along that line | 位于那条线上的某个位置 |

语法补充：

- **a 45-degree angle**：一个 45 度角。`45-degree` 是复合形容词，因此 degree 使用单数。
- **The angle is 45 degrees.**：这个角是 45 度。此时 degrees 使用复数。

---

## 11. 一句话总结

Roofline 用“处理器计算峰值水平线”和“内存带宽斜线”共同构成性能屋顶；给定 kernel 的运算强度后，从 X 轴向上找到屋顶，就能判断其理论性能上限以及更可能属于计算受限还是内存受限。
