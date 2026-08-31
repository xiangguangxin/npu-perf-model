#pragma once
// 统计与输出 —— MVP-4：在访存/计算指标之外，补上 contention 与 fairness。
//   - 访存：bytes、achieved BW、有效带宽 = min(interconnect, hbm)
//   - 计算：throughput、utilization、AI（roofline）
//   - 竞争：平均排队延迟、背压 stall、queue-full 事件、每源授予次数（公平性）
// 有了 AI 与吞吐，就能做 roofline 定位 compute-bound / memory-bound。

#include "common.h"
#include "interconnect.h"
#include "memory_controller.h"
#include "hbm.h"
#include "pe_array.h"
#include "workload_driver.h"

namespace npu_perf {

class PerfMonitor {
public:
    // 理想最小搬运字节（无复用惩罚）
    static double bytes_min(const NpuConfig& c, const GemmTask& t);

    static void report(const NpuConfig& cfg, const GemmTask& t,
                       const Interconnect& ic, const MemoryController& mc,
                       const Hbm& hbm, const PeArray& pe,
                       const WorkloadDriver& drv);
};

}  // namespace npu_perf
