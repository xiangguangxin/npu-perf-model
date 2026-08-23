#pragma once
// ============================================================================
// Memory —— MVP-3 的 HBM AT target（延迟 + 带宽抽象）。
//
// 【MVP-3 框架图：目标端的时序资源】
//
// DMA BEGIN_REQ ──> nb_transport_fw()
//                       │  arrival = now + annotated delay
//                       │  ready_for_data = arrival + hbm_lat
//                       ├─ PEQ: END_REQ   @ arrival（释放 DMA 请求通道）
//                       └─ next_data_free_ = max(ready_for_data, old next_data_free_) + bytes/bw
//                                  │
//                                  └─ PEQ: BEGIN_RESP @ data_done
//                                               │
// DMA END_RESP <── nb_transport_fw() <── peq_cb() <── nb_transport_bw()
//
// 两条建模规则刻意分开：固定延迟可以被多笔 outstanding 请求重叠；真正占用 HBM
// 数据通道的 bytes/bw 则由 next_data_free_ 串行化。因此提交更多请求能隐藏 latency，
// 但不能超过 hbm_bw_GBps 的带宽上限。
// ============================================================================

#include "common.h"
#include <tlm_utils/peq_with_cb_and_phase.h>

namespace npu_perf {

class Memory : public sc_module {
public:
    tlm_utils::simple_target_socket<Memory> tsock;   // 对外可绑定，故留 public

    SC_HAS_PROCESS(Memory);
    Memory(sc_module_name n, NpuConfig c);

    // 接收 BEGIN_REQ，计算并登记两个反向 phase 的未来发生时间；接收 END_RESP 时
    // 返回 TLM_COMPLETED，表示 target 不再持有该 payload。
    tlm_sync_enum nb_transport_fw(tlm_generic_payload& gp, tlm_phase& phase,
                                  sc_time& delay);

    // ---- 统计只读访问器 ----
    uint64_t serviced_reqs()  const { return serviced_reqs_; }   // 处理过的事务数
    uint64_t serviced_bytes() const { return serviced_bytes_; }  // 累计搬运字节

private:
    NpuConfig cfg_;
    // PEQ = 延迟投递器：将 END_REQ / BEGIN_RESP 的发送安排在未来的仿真时刻，
    // 到点自动回调 peq_cb，避免在 nb_transport_fw 内直接嵌套调用 nb_transport_bw。
    // 注意：PEQ 不持有 payload，由 initiator 负责保活到 END_RESP 闭合为止。
    tlm_utils::peq_with_cb_and_phase<Memory> peq_;
    // HBM 数据通道最早可用时间。不是“响应时间”：固定延迟可在此之前并行倒计时。
    sc_time next_data_free_ = SC_ZERO_TIME;
    uint64_t serviced_reqs_  = 0;
    uint64_t serviced_bytes_ = 0;

    // PEQ 到期后的 backward-path 发送点：END_REQ 或 BEGIN_RESP。
    void peq_cb(tlm_generic_payload& gp, const tlm_phase& phase);
};

}  // namespace npu_perf
