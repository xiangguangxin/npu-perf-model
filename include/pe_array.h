#pragma once
// ============================================================================
// PeArray —— weight-stationary 脉动阵列的 timing 模型（cycle-approximate）。
//
// 【建模什么】只建 timing，不做真实 MAC。一个 N×N 阵列算一趟(pass)的耗时，
//   按脉动阵列经典的三阶段：
//     fill(填充)   ~N cycle：激活波前从边缘传播、逐步填满阵列
//     steady(稳态) ~N cycle：N 个激活向量依次流过，每拍产出一列结果
//     drain(排空)  ~N cycle：最后的部分和排出阵列
//   一趟 = 权重(N×N)驻留 + N 个激活向量流过，产出 N×N 输出、做 N³ 次 MAC。
//   pass_time = fill + steady + drain = 3N cycle。
//
//   注意 3N 里只有 steady 那 N cycle 在满负荷，fill/drain 是流水线开销——
//   所以单趟利用率约 1/3。真实设计靠"权重驻留 + 流很多激活"把 fill/drain
//   摊薄，这也是后续调 tile/dataflow 能提利用率的实验点。
//
// 【纯 timing 辅助模块】和 OnchipBuffer 一样不挂 socket，由 Driver 直接查询。
//   MVP-2 先串行接入(load→compute→store)；让 compute 与下一块 DMA 预取
//   重叠(double buffering) 是 MVP-2 的下一步。
// ============================================================================

#include "common.h"

class PeArray : public sc_module {
public:
    PeArray(sc_module_name n, NpuConfig c) : sc_module(n), cfg_(c) {}

    // 算一趟的时间 = fill + steady + drain = 3N cycle
    sc_time pass_time() const;

    // 登记一趟做了多少 MAC（N³），供吞吐/利用率统计
    void account_pass();

    // ---- 只读访问器 ----
    uint64_t passes() const { return passes_; }        // 累计趟数
    uint64_t macs()   const { return macs_; }          // 累计 MAC 次数
    uint64_t flops()  const { return 2ull * macs_; }   // 1 MAC = 2 FLOP

private:
    NpuConfig cfg_;
    uint64_t passes_ = 0;
    uint64_t macs_   = 0;
};
