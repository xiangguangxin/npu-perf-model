// Memory 的实现：HBM 的 AT target，以 PEQ 调度四相协议的反向阶段。
#include "memory.h"
#include <algorithm>

Memory::Memory(sc_module_name n, NpuConfig c)
  : sc_module(n), tsock("tsock"), cfg_(c),
    peq_(this, &Memory::peq_cb) {
    tsock.register_nb_transport_fw(this, &Memory::nb_transport_fw);
}

// 前向路径入口：接收 initiator 发来的 phase，计算 HBM 延迟与带宽时序，将后向事件投递到 PEQ。
//
// DMA ──BEGIN_REQ──▶ nb_transport_fw()
//                       │
//                       ├─ ① 校验 phase（只接受 BEGIN_REQ 或 END_RESP）
//                       │
//                       ├─ ② 计算时间线：
//                       │     arrival        = now + delay              (请求到达时刻)
//                       │     ready_for_data = arrival + HBM 延迟       (数据就绪时刻)
//                       │     data_start     = max(ready_for_data, 上次传输结束)
//                       │     data_done      = data_start + bytes/带宽  (传输完成时刻)
//                       │
//                       ├─ ③ 投递 PEQ 事件：
//                       │     END_REQ    @ arrival   ──▶ DMA 收到后可发下一笔
//                       │     BEGIN_RESP @ data_done ──▶ DMA 收到后回 END_RESP
//                       │
//                       └─ ④ 返回 TLM_ACCEPTED（走异步四相路径）
tlm_sync_enum Memory::nb_transport_fw(tlm_generic_payload& gp, tlm_phase& phase, sc_time& delay) {
    // END_RESP 是 initiator 对 BEGIN_RESP 的确认；到这里 target 不再保留 payload。
    if (phase == END_RESP) return TLM_COMPLETED;
    if (phase != BEGIN_REQ) {
        SC_REPORT_ERROR("Memory", "unexpected forward phase");
        return TLM_COMPLETED;
    }

    const double bytes = double(gp.get_data_length());
    // 允许上游携带 annotation；本工程当前 DMA 传 0，但按 AT 语义仍需计算到达时刻。
    const sc_time arrival = sc_time_stamp() + delay;
    const sc_time ready_for_data = arrival + cycles(cfg_.hbm_lat_cyc());

    // 固定 latency 期间不占用数据通道；若前一笔传输尚未结束，新请求必须排队，
    // 从而让 bytes/bw 的资源约束全局生效，而不是每个请求各自“满带宽”。
    //     Tile0:
    //      ready_for_data = 100 ns
    //      data_done      = 140 ns
    //      next_data_free_ = 140 ns

    //     Tile1:
    //      ready_for_data = 120 ns
 
    //      data_start = max(120 ns, 140 ns) = 140 ns
    //     时间：
    //       100ns                140ns                 180ns
    //         │                    │                     │
    //         ├────── Tile0 ───────┤                     │
    //         │                    ├────── Tile1 ────────┤
    //         │                    │                     │
    //                           Tile0完成
    //                           Tile1开始
    //     Tile1 虽然 120ns 已经 ready，但 HBM 数据通道仍被 Tile0 占用，所以必须等到 next_data_free_=140ns。因此：
    //       data_start = max(ready_for_data, next_data_free_)
    //     表示：
    //       “数据已经准备好” 和 “数据通道空闲”， 两个条件都满足后，才能真正开始传输。
    //  ready_for_data 决定“我自己准备好了没有”，next_data_free_ 决定“公共数据通道有没有空”，两者都满足才能 data_start
    const sc_time data_start = std::max(ready_for_data, next_data_free_);

    // 传输时间 = 数据量 / 带宽
    const sc_time data_done = data_start + sc_time(bytes / cfg_.hbm_bw_Bps(), SC_SEC);
    next_data_free_ = data_done;

    // 将两个后向 phase 异步投递到 PEQ，到点自动回调 peq_cb，避免在 fw 回调中重入。
    // END_REQ：请求到达 Memory 后立即异步返回，表示 Memory 已接受该请求。
    //   注意：END_REQ 不代表数据传输完成，outstanding_ 仍保持占用， 真正完成并释放 DMA outstanding slot 的时刻是 BEGIN_RESP → END_RESP → complete()。
    peq_.notify(gp, END_REQ, delay);                          // 第 2 拍：请求到达即发，释放 DMA 窗口

    // BEGIN_RESP：等 HBM latency + 带宽传输完成后异步返回，data_done 表示本次 Tile 数据传输完成时刻。
    peq_.notify(gp, BEGIN_RESP, data_done - sc_time_stamp()); // 第 3 拍：数据传完才发
    
    serviced_reqs_  += 1;
    serviced_bytes_ += uint64_t(bytes);
    gp.set_response_status(TLM_OK_RESPONSE);
    return TLM_ACCEPTED;
}

// peq_ 的到期回调：notify(gp, phase, when) 在 when 时间后自动调用本函数。
// 职责很薄——只是把 phase 原样转发给 initiator 的后向路径，不做额外处理。
void Memory::peq_cb(tlm_generic_payload& gp, const tlm_phase& phase) {
    tlm_phase bw_phase = phase;
    sc_time delay = SC_ZERO_TIME;
    const tlm_sync_enum status = tsock->nb_transport_bw(gp, bw_phase, delay);
    // DmaEngine 收到 BEGIN_RESP 后会同步回 END_RESP，返回 TLM_COMPLETED。
    // 若未来接入延迟 END_RESP 的 initiator，需继续追踪 phase 而非直接报错。
    if (phase == BEGIN_RESP && status != TLM_COMPLETED) {
        SC_REPORT_ERROR("Memory", "initiator did not complete BEGIN_RESP");
    }
}
