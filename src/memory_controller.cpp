// MemoryController 的实现：插在 Interconnect 与 Hbm 之间的带宽 bridge。
#include "memory_controller.h"
#include <algorithm>

namespace npu_perf {

MemoryController::MemoryController(sc_module_name n, NpuConfig c)
  : sc_module(n), tsock("tsock"), isock("isock"), cfg_(c),
    peq_(this, &MemoryController::peq_cb) {
    tsock.register_nb_transport_fw(this, &MemoryController::nb_transport_fw);
    isock.register_nb_transport_bw(this, &MemoryController::nb_transport_bw);
}

// 前向路径：来自 Interconnect。
tlm_sync_enum MemoryController::nb_transport_fw(tlm_generic_payload& gp,
                                                tlm_phase& phase, sc_time& delay) {
    if (phase == END_RESP) {
        // initiator 收尾：转发给 Hbm 闭合协议（Hbm 返回 TLM_COMPLETED）。
        tlm_phase p = END_RESP;
        isock->nb_transport_fw(gp, p, delay);
        return TLM_COMPLETED;
    }
    if (phase != BEGIN_REQ) {
        SC_REPORT_ERROR("MemoryController", "unexpected forward phase");
        return TLM_COMPLETED;
    }

    // 收到 BEGIN_REQ：直接转发给 Hbm（固定延迟由 Hbm 负责，带宽在响应阶段串行化）。
    tlm_phase p = BEGIN_REQ;
    isock->nb_transport_fw(gp, p, delay);
    serviced_reqs_  += 1;
    serviced_bytes_ += gp.get_data_length();
    return TLM_ACCEPTED;
}

// 后向路径：来自 Hbm。
tlm_sync_enum MemoryController::nb_transport_bw(tlm_generic_payload& gp,
                                                tlm_phase& phase, sc_time& delay) {
    if (phase == END_REQ) {
        // Hbm 已接受：转发 END_REQ 给 Interconnect。
        tlm_phase p = END_REQ;
        tsock->nb_transport_bw(gp, p, delay);
        return TLM_ACCEPTED;
    }
    if (phase == BEGIN_RESP) {
        // Hbm 数据就绪（此刻 = 到达 + hbm_lat）。在这里串行化带宽：
        //   data_start = max(now, next_data_free_)   —— 数据就绪 且 通道空闲
        //   data_done  = data_start + bytes/bw        —— 真正传完
        // 这与 MVP-3 的 Memory 里 ready_for_data/next_data_free_ 公式一致。
        const double bytes = double(gp.get_data_length());
        const sc_time data_start = std::max(sc_time_stamp(), next_data_free_);
        const sc_time data_done  = data_start + sc_time(bytes / cfg_.hbm_bw_Bps(), SC_SEC);
        next_data_free_ = data_done;

        // BEGIN_RESP 延迟到 data_done 再送回 Interconnect（PEQ 避免重入）。
        peq_.notify(gp, BEGIN_RESP, data_done - sc_time_stamp());
        return TLM_ACCEPTED;
    }

    SC_REPORT_ERROR("MemoryController", "unexpected backward phase");
    return TLM_COMPLETED;
}

// PEQ 到期回调：把 BEGIN_RESP 送回 Interconnect。
void MemoryController::peq_cb(tlm_generic_payload& gp, const tlm_phase& phase) {
    tlm_phase bw_phase = phase;
    sc_time   delay = SC_ZERO_TIME;
    const tlm_sync_enum status = tsock->nb_transport_bw(gp, bw_phase, delay);
    if (phase == BEGIN_RESP && status != TLM_COMPLETED) {
        SC_REPORT_ERROR("MemoryController", "initiator did not complete BEGIN_RESP");
    }
}

}  // namespace npu_perf
