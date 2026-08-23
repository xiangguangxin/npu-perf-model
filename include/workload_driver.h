#pragma once
// ============================================================================
// WorkloadDriver —— 整个仿真的 "总指挥"。
//
// MVP-2：在 MVP-1 数据流基础上接入 PE 计算。把 GEMM 按 output tile 分块，每个
// output tile 在 K 方向累加：逐个 K-slice 搬权重+激活 → PE 算一趟，累加完写回。
// 两种调度由 NpuConfig.double_buffer 切换（构造时按需注册 SC_THREAD）：
//   - 串行(serial)     ：单线程 run_serial()，load→compute→store 完全不重叠，对照下界
//   - 双缓冲(double buf)：两条 SC_THREAD 真并发 —— loader 预取、compute 消费，
//                         用两块 ping-pong 载入槽 + sc_event 做生产者-消费者同步，
//                         load 与 compute 在仿真时间里真正重叠（还能跨 output tile 预取）。
//
// 【系统设计框图 —— 模块拓扑】
//   两种连接方式要分清：
//     ═══>  TLM socket 绑定，走 AT 四相总线协议，真正的数据搬运通道
//     ──>   普通 C++ 函数调用(拿指针直接使唤)，用于查询/发号施令
//
//        ┌─────────────────────────────────────────────┐
//        │   WorkloadDriver                            │  串行:1 线程 run_serial
//        │   双缓冲: loader ⇄(ping-pong+event)⇄ compute │  双缓冲:2 线程并发
//        └──┬──────────────┬───────────────────┬───────┘
//   dma_->rw │(函数调用)    │ buf_->access_time │ pe_->pass_time (函数调用)
//            ▼              ▼                   ▼
//     ┌──────────────┐ ┌──────────────┐ ┌──────────────┐
//     │  DmaEngine   │ │ OnchipBuffer │ │   PeArray    │ 纯 timing，不挂 socket
//     │ read()/write │ │ 容量+带宽     │ │ fill/steady/ │ (Driver 直接查询)
//     └──────┬───────┘ └──────────────┘ │    drain     │
//      isock │═══ nb_transport (AT/四相) ═╗└──────────────┘
//            ▼                           ║
//     ┌──────────────┐                   ║ 只有这一条是 socket 通道
//     │    Memory    │ ◄═════════════════╝
//     │ HBM: 延迟+带宽│  (MVP-2 仍单 DMA 直连，Interconnect 是加分项)
//     └──────────────┘
//
//   仿真结束后，PerfMonitor 读取各模块统计量(run_time/bytes_moved/macs...) 出报告。
//
// 【双缓冲的同步（正是 delta-notify + 事件条件等待的实战）】
//   两块槽：free_slots_ 空槽数(初始 2)、filled_slots_ 满槽数。
//     loader ：while(无空槽) wait(ev_free) → 占槽、搬两操作数、filled++、notify(ev_filled)
//     compute：while(无满槽) wait(ev_filled) → 取槽、PE 算、释放、free++、notify(ev_free)
//   loader 最多领先 compute 两块（受 2 槽限制），从而 load(k+1) 与 compute(k) 真重叠。
//   compute 是下游、总最后结束，由它记 run_time_。不调 sc_stop()：线程跑完、无 pending
//   事件时 sc_start 自然返回（也让多 driver 能在一次 sc_start 里各自独立跑完，测试用）。
// ============================================================================

#include "common.h"
#include "dma_engine.h"
#include "onchip_buffer.h"
#include "pe_array.h"

namespace npu_perf {

class WorkloadDriver : public sc_module {
public:
    SC_HAS_PROCESS(WorkloadDriver);
    WorkloadDriver(sc_module_name n, NpuConfig c, GemmTask t,
                   DmaEngine* d, OnchipBuffer* b, PeArray* p);

    // 数据流+计算总耗时（供 PerfMonitor 读取）
    sc_time run_time() const { return run_time_; }

private:
    static constexpr int DB_SLOTS = 2;   // 双缓冲的 ping-pong 槽数
    // [ 单缓冲区模式 (Serial) ]
    // 时间 ───>
    // DMA 搬运 Tile 0 ───► PE 阵列计算 Tile 0 ───► DMA 搬运 Tile 1 ───► PE 阵列计算 Tile 1
    //                       (此时 DMA 闲置)                             (此时 PE 闲置)

    // [ 乒乓双缓冲模式 (Double Buffer) ]
    // 时间 ───>
    // Slot 0:  [ DMA 搬运 Tile 0 ] ───► [  PE 算 Tile 0  ] ───► [ DMA 搬运 Tile 2 ]
    // Slot 1:                          [ DMA 搬运 Tile 1 ] ───► [  PE 算 Tile 1  ]
    //                                  (搬运与计算同时进行)


    static uint32_t ceil_div(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
    
    // 计算单个矩阵分块（Tile）包含的数据字节数（Byte）
    // 当前的切片（Tile）尺寸完全等同于物理脉动阵列的大小
    uint32_t tile_bytes() const {
        // Tile 字节数 = 脉动阵列边长N X 脉动阵列边长N X 单个元素的字节数
        return cfg_.array_n() * cfg_.array_n() * cfg_.data_bytes();
    }

    // load 一块 tile：HBM -> 片上（HBM 延迟+带宽，再加写进 buffer 的带宽时间）
    void load_tile(TileExtension::Kind kind, uint32_t bytes, uint32_t tid);
    // store 一块 output tile：片上 -> HBM（先从 buffer 读出，再经 DMA 写回）
    void store_tile(uint32_t bytes, uint32_t tid);

    // 串行调度：搬和算完全不重叠（对照下界）
    void run_serial();
    // 双缓冲：生产者线程，按 (i,j,k) 顺序把每个 K-slice 载入一个空槽。
    void loader();
    // 双缓冲：消费者线程，取满槽做 PE 计算；每个 output tile 的 K 累加完负责写回。
    void compute();

    NpuConfig     cfg_;
    GemmTask      task_;
    DmaEngine*    dma_;
    OnchipBuffer* buf_;
    PeArray*      pe_;
    sc_time       run_time_ = SC_ZERO_TIME;

    // 双缓冲同步状态
    int      free_slots_   = DB_SLOTS;   // 空槽数
    int      filled_slots_ = 0;          // 满槽数（已载入、待计算）
    sc_event ev_free_;                   // compute → loader：腾出空槽
    sc_event ev_filled_;                 // loader → compute：填好满槽
};

}  // namespace npu_perf
