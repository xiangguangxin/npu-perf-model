#pragma once
// ============================================================================
// NpuSystem —— MVP-4 顶层组装（对应类图里的 NPU_System sc_module）。
//
// 【拓扑（MVP4-Class-Diagram §1 / §7）】
//     WorkloadDriver（分块调度，请求 round-robin 发到 N 个 DMA）
//         │ 函数调用
//         ├─► DmaEngine[0..N-1].isock ──AT──► Interconnect.dma_socket(i)
//         │                                      │ 队列 + 仲裁 + 背压
//         │                                      ▼
//         │                              Interconnect.mc_socket ──AT──► MemoryController
//         │                                                              │ 带宽串行化
//         │                                                              ▼
//         │                                                       MemoryController.isock ──AT──► Hbm
//         │                                                                                   │ 固定延迟
//         └─► OnchipBuffer / PeArray（timing 辅助，无 socket）
//
// 把原来散落在 main.cpp 的模块实例化与绑定收拢到一处，main.cpp 只负责解析参数。
// ============================================================================

#include "common.h"
#include "hbm.h"
#include "memory_controller.h"
#include "interconnect.h"
#include "onchip_buffer.h"
#include "dma_engine.h"
#include "pe_array.h"
#include "workload_driver.h"
#include <vector>
#include <memory>

namespace npu_perf {

class NpuSystem {
public:
    NpuSystem(const char* name, NpuConfig c, GemmTask t);

    // 跑完整仿真（sc_start 会等所有 SC_THREAD 结束）。
    void run() { sc_core::sc_start(); }

    // ---- 供 PerfMonitor 读取的只读视图 ----
    const Interconnect&     interconnect() const { return ic_; }
    const MemoryController& mc()          const { return mc_; }
    const Hbm&              hbm()         const { return hbm_; }
    const PeArray&          pe()          const { return pe_; }
    const WorkloadDriver&   driver()      const { return *drv_; }
    uint32_t                dma_count()   const { return uint32_t(dmas_.size()); }

private:
    NpuConfig cfg_;
    GemmTask  task_;
    Hbm               hbm_;
    MemoryController  mc_;
    Interconnect      ic_;
    OnchipBuffer      buf_;
    PeArray           pe_;
    std::vector<std::unique_ptr<DmaEngine>> dmas_;      // 拥有 DMA
    std::vector<DmaEngine*>                 dma_ptrs_;  // 裸指针给 driver
    std::unique_ptr<WorkloadDriver>         drv_;
};

}  // namespace npu_perf
