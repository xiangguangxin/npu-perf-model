#pragma once
// 统计与输出 —— MVP-2：在访存侧指标之外，补上计算侧的吞吐/利用率/AI。
//   - 访存：bytes、achieved BW、解析 T_memory
//   - 计算：throughput(FLOP/s)、utilization(=实际/峰值)、AI(arithmetic intensity)
// 有了 AI 与吞吐，就能做 roofline 定位 compute-bound / memory-bound。

#include "common.h"
#include "memory.h"
#include "dma_engine.h"
#include "pe_array.h"
#include "workload_driver.h"

namespace npu_perf {

class PerfMonitor {
public:
    // 理想最小搬运字节（无复用惩罚）
    static double bytes_min(const NpuConfig& c, const GemmTask& t);

    static void report(const NpuConfig& cfg, const GemmTask& t,
                       const Memory& mem, const DmaEngine& dma,
                       const PeArray& pe, const WorkloadDriver& drv);
};

}  // namespace npu_perf
