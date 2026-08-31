// Hbm 的实现：固定延迟 target，以 PEQ 调度四相协议的反向阶段。
#include "hbm.h"

namespace npu_perf {

Hbm::Hbm(sc_module_name n, NpuConfig c)
  : sc_module(n), tsock("tsock"), cfg_(c),
    peq_(this, &Hbm::peq_cb) {
    tsock.register_nb_transport_fw(this, &Hbm::nb_transport_fw);
}

// 前向路径：接收 BEGIN_REQ，把固定延迟后的反向事件投递到 PEQ。
//
//   MC ──BEGIN_REQ──▶ nb_transport_fw()
//                       ├─ arrival        = now + delay           (到达时刻)
//                       ├─ ready_for_data = arrival + hbm_lat      (数据就绪)
//                       ├─ PEQ: END_REQ    @ arrival  （释放上游请求通道）
//                       └─ PEQ: BEGIN_RESP @ ready     （数据就绪，交回 MC 串行带宽）
tlm_sync_enum Hbm::nb_transport_fw(tlm_generic_payload& gp, tlm_phase& phase,
                                   sc_time& delay) {
    if (phase == END_RESP) return TLM_COMPLETED;   // initiator 对 BEGIN_RESP 的确认
    if (phase != BEGIN_REQ) {
        SC_REPORT_ERROR("Hbm", "unexpected forward phase");
        return TLM_COMPLETED;
    }

    const sc_time arrival        = sc_time_stamp() + delay;
    const sc_time ready_for_data = arrival + cycles(cfg_.hbm_lat_cyc());

    // 反向 phase 异步投递到 PEQ（不直接嵌套调用 backward，避免重入）。
    peq_.notify(gp, END_REQ,    delay);                        // @ arrival
    peq_.notify(gp, BEGIN_RESP, ready_for_data - sc_time_stamp()); // @ ready

    serviced_reqs_  += 1;
    serviced_bytes_ += gp.get_data_length();
    gp.set_response_status(TLM_OK_RESPONSE);
    return TLM_ACCEPTED;
}

// PEQ 到期回调：把 phase 原样转发给上游 initiator（MemoryController）的 backward 路径。
void Hbm::peq_cb(tlm_generic_payload& gp, const tlm_phase& phase) {
    tlm_phase bw_phase = phase;
    sc_time   delay = SC_ZERO_TIME;
    const tlm_sync_enum status = tsock->nb_transport_bw(gp, bw_phase, delay);
    if (phase == BEGIN_RESP && status != TLM_COMPLETED) {
        SC_REPORT_ERROR("Hbm", "initiator did not complete BEGIN_RESP");
    }
}

}  // namespace npu_perf
