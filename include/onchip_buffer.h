#pragma once
// ============================================================================
// OnchipBuffer —— 片上 SRAM（又快又近、但容量很小的暂存区）的 timing 模型。
//
// 【在数据流里的位置】
//     HBM ──经 MC/Interconnect/DMA 搬运──> [OnchipBuffer] ──喂──> PE Array
//                  第①段路                         第②段路
//   数据被 DMA 从慢速 HBM 搬到片上后，先落在这里暂存，再喂给计算单元。
//   本模块建模的是 "第②段"：数据到了片上之后，写进 SRAM 这块存储本身的开销，
//   而不是 "从 HBM 搬过来"那段（HBM 固定延迟由 Hbm 建模，
//   HBM 数据通道带宽由 MemoryController 串行化，互连时序由 Interconnect 建模）。
//
// 【建模两道约束，缺一不可 —— 这是性能模型的精髓】
//   1) 容量约束(can_hold)  : 片上就这么大(buffer_bytes)，装不下要先腾空。
//                            正是需要把大矩阵切成 tile 逐块进出的根本原因。
//   2) 带宽约束(access_time): 数据即使到了片上，写进 SRAM 也不是瞬时的，
//                            每 cycle 只能吞 buf_bw_Bpc 字节。
//
// 【建模的简化（perf 模型只关心 "占多少、花多久"）】
//   - 不存真实数据：只记账占用字节数，不落地矩阵内容。
//   - 不挂 TLM socket：不是被搬运数据的 target，由 Driver 直接函数调用查询。
//   - 不建复用/替换策略：MVP-1 逐块 allocate→release，进出即走；真实 NPU
//     会把数据尽量留在 buffer 里反复用以减少 HBM 访问，那套留到后续阶段。
// ============================================================================

#include "common.h"

namespace npu_perf {

class OnchipBuffer : public sc_module {
public:
    OnchipBuffer(sc_module_name n, NpuConfig c) : sc_module(n), cfg_(c) {}

    // ---- 容量约束 ----
    // 再塞 bytes 字节会不会超过 buffer 总容量？(uint64_t 相加防 32 位溢出)
    bool can_hold(uint32_t bytes) const;
    // 占位：登记又用掉了 bytes，并顺手更新历史占用峰值(供敏感性分析)。
    void allocate(uint32_t bytes);
    // 腾空：释放 bytes；防下溢，释放量 >= 当前占用时直接清零。
    void release(uint32_t bytes);

    // ---- 带宽约束 ----
    // 读/写一块 bytes 数据占用的时间 = ceil(bytes / buffer 带宽) 个 cycle。
    // buf_bw_Bpc = 每 cycle 能吞多少字节(默认 64)，向上取整因为不足一 cycle 也占满一 cycle。
    //   例：256 字节 @ 64 B/cycle → ceil(256/64)=4 cycle = 4ns。
    // MVP-1 用在 "数据从 DMA 搬来后写进 buffer" 这一步；同一模型也可算 "从 buffer
    // 读出喂给 PE" 的时间(读同样受此带宽限制)。
    sc_time access_time(uint32_t bytes) const;

    // ---- 只读访问器（外部只能看、不能改，防止误改破坏统计）----
    uint32_t used_bytes() const { return used_bytes_; }   // 当前占用字节数
    uint32_t peak_bytes() const { return peak_bytes_; }   // 统计：历史占用峰值

private:
    NpuConfig cfg_;               // 硬件配置(容量、带宽从这里取)
    uint32_t used_bytes_ = 0;     // 当前已用字节
    uint32_t peak_bytes_ = 0;     // 占用峰值(高水位线)
};

}  // namespace npu_perf
