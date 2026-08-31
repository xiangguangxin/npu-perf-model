#pragma once
// ============================================================================
// MemoryController —— MVP-4 中插在 Interconnect 与 Hbm 之间的 bridge（§7）。
//
// 【职责】内存带宽串行化 + 调度。固定延迟已交给 Hbm，这里只做"数据通道"这一道：
//   - 请求队列（backpressure 的上游由 Interconnect 负责，本处不再限深）；
//   - 调度：data_start = max(Hbm 就绪时刻, 上一次传输结束)；
//   - 带宽计算：data_done = data_start + bytes / mc_bw。
//
// 【为什么拆成两段】把固定延迟与带宽通道分开后，两者各自独立：
//   - Hbm 的 hbm_lat 可被多笔 outstanding 请求并行重叠；
//   - MC 的 bytes/bw 用 next_data_free_ 串行化，保证全局带宽上限。
//   单 DMA + 默认参数下，两者合起来的时序与 MVP-3 的 Memory 完全一致。
//
// 【四相 bridge】上游 target socket 收 Interconnect，下游 initiator socket 发 Hbm：
//     BEGIN_REQ  （fw）：转发给 Hbm；
//     END_REQ    （bw）：转发给 Interconnect；
//     BEGIN_RESP （bw）：Hbm 就绪后，串行带宽再经 PEQ 延迟送回 Interconnect；
//     END_RESP   （fw）：转发给 Hbm 闭合。
// ============================================================================

#include "common.h"
#include <tlm_utils/peq_with_cb_and_phase.h>

namespace npu_perf {

class MemoryController : public sc_module {
public:
    tlm_utils::simple_target_socket<MemoryController>    tsock;   // 来自 Interconnect
    tlm_utils::simple_initiator_socket<MemoryController> isock;   // 发往 Hbm

    SC_HAS_PROCESS(MemoryController);
    MemoryController(sc_module_name n, NpuConfig c);

    // 前向：来自 Interconnect。
    tlm_sync_enum nb_transport_fw(tlm_generic_payload& gp, tlm_phase& phase,
                                  sc_time& delay);
    // 后向：来自 Hbm。
    tlm_sync_enum nb_transport_bw(tlm_generic_payload& gp, tlm_phase& phase,
                                  sc_time& delay);

    // ---- 统计只读访问器 ----
    uint64_t serviced_reqs()  const { return serviced_reqs_; }
    uint64_t serviced_bytes() const { return serviced_bytes_; }

private:
    NpuConfig cfg_;
    tlm_utils::peq_with_cb_and_phase<MemoryController> peq_;
    // 数据通道最早可用时间（带宽串行化，不是响应时间）。
    sc_time next_data_free_ = SC_ZERO_TIME;
    uint64_t serviced_reqs_  = 0;
    uint64_t serviced_bytes_ = 0;

    void peq_cb(tlm_generic_payload& gp, const tlm_phase& phase);
};

}  // namespace npu_perf
