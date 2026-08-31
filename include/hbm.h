#pragma once
// ============================================================================
// Hbm —— MVP-4 的 HBM 存储体 target（只建模固定访问延迟）。
//
// 【MVP-4 里 Hbm 的职责被"减负"了】MVP-3 的 Memory 同时做延迟+带宽；MVP-4 把
// 带宽串行化拆给了 MemoryController（见 memory_controller.h），Hbm 只剩：
//   - 固定访问延迟 hbm_lat_cyc（可被多笔 outstanding 请求重叠）；
//   - 四相 AT 里 BEGIN_REQ 之后的 END_REQ / BEGIN_RESP 调度。
//
// 【数据就绪时刻】请求到达 → 再等 hbm_lat 后数据就绪（BEGIN_RESP）。
//   固定延迟期间不占用任何共享通道，因此多笔请求可并行倒计时——这正是
//   MVP-3 里"outstanding 隐藏 HBM 延迟"的由来，只是带宽部分挪到了 MC。
//
// 【拓扑位置】MemoryController.isock ──AT──> Hbm.tsock（最末端 target）。
// ============================================================================

#include "common.h"
#include <tlm_utils/peq_with_cb_and_phase.h>

namespace npu_perf {

class Hbm : public sc_module {
public:
    tlm_utils::simple_target_socket<Hbm> tsock;   // 对外可绑定，故留 public

    SC_HAS_PROCESS(Hbm);
    Hbm(sc_module_name n, NpuConfig c);

    // 接收 BEGIN_REQ，登记 END_REQ@到达、BEGIN_RESP@到达+latency；接收 END_RESP 时返回完成。
    tlm_sync_enum nb_transport_fw(tlm_generic_payload& gp, tlm_phase& phase,
                                  sc_time& delay);

    // ---- 统计只读访问器 ----
    uint64_t serviced_reqs()  const { return serviced_reqs_; }
    uint64_t serviced_bytes() const { return serviced_bytes_; }

private:
    NpuConfig cfg_;
    // PEQ 延迟投递 END_REQ / BEGIN_RESP，避免在 fw 回调内重入 backward 路径。
    tlm_utils::peq_with_cb_and_phase<Hbm> peq_;
    uint64_t serviced_reqs_  = 0;
    uint64_t serviced_bytes_ = 0;

    void peq_cb(tlm_generic_payload& gp, const tlm_phase& phase);
};

}  // namespace npu_perf
