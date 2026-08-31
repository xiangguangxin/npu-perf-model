// NpuSystem 的实现：MVP-4 顶层模块的实例化与 socket 绑定。
#include "npu_system.h"

namespace npu_perf {

NpuSystem::NpuSystem(const char* name, NpuConfig c, GemmTask t)
  : cfg_(c), task_(t),
    hbm_((std::string(name) + "_hbm").c_str(), c),
    mc_ ((std::string(name) + "_mc").c_str(),  c),
    ic_ ((std::string(name) + "_ic").c_str(),  c, c.dma_count()),
    buf_((std::string(name) + "_buf").c_str(), c),
    pe_ ((std::string(name) + "_pe").c_str(),  c) {
    // N 个 DMA 引擎，各自绑定到 Interconnect 的一个 tagged target socket。
    for (uint32_t i = 0; i < c.dma_count(); ++i) {
        auto dma_name = std::string(name) + "_dma" + std::to_string(i);
        auto dma = std::make_unique<DmaEngine>(dma_name.c_str(), c);
        dma_ptrs_.push_back(dma.get());
        dmas_.push_back(std::move(dma));
    }
    for (uint32_t i = 0; i < c.dma_count(); ++i)
        dmas_[i]->isock.bind(ic_.dma_socket(i));

    // 互连 → 内存控制器 → HBM。
    ic_.mc_socket().bind(mc_.tsock);
    mc_.isock.bind(hbm_.tsock);

    // 调度器持有 N 个 DMA（round-robin 分发请求）。
    drv_ = std::make_unique<WorkloadDriver>(
        (std::string(name) + "_drv").c_str(), c, t, dma_ptrs_, &buf_, &pe_);
}

}  // namespace npu_perf
