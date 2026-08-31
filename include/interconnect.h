#pragma once
// ============================================================================
// Interconnect —— MVP-4 的单跳共享互连（§5）。
//
// 【职责】把 N 个 DMA 的请求汇聚，做排队 + 仲裁 + 背压 + 网络延迟，再转给
//   MemoryController。是整个 contention 模型的核心，也是唯一需要"多源路由"的
//   模块（其他模块都是单上游/单下游）。
//
// 【拓扑】
//     DMA[0].isock ─┐
//     DMA[1].isock ─┼─► Interconnect.tsock[i]（每个 DMA 一个 tagged socket）
//     ...            │        │ 队列 + 仲裁 + 背压
//     DMA[N].isock ─┘        ▼
//                    Interconnect.isock ──AT──► MemoryController.tsock
//
// 【为什么用 tagged socket】每个 DMA 绑到不同的 target socket，socket 用 id 打标，
//   nb_transport_fw 回调带 source 参数，从而把响应路由回正确的 DMA。
//
// 【时序（对应 §8）】
//     Tqueue        = 请求在互连队列里等待仲裁/带宽的时间
//     Tinterconnect = noc_latency（固定单跳延迟）+ bytes/interconnect_bw（转发节拍）
//   互连带宽在单条共享总线上串行化转发；下游 MC 再做 bytes/hbm_bw 串行化。
//   二者流水线叠加，有效带宽 = min(interconnect_bw, hbm_bw)。
//
// 【背压（§9）】队列深度有限。队列满时新请求进入 blocked_ 等待（不回 END_REQ），
//   直到有槽腾出再 promote。统计 stall 时间与 queue-full 事件。
// ============================================================================

#include "common.h"
#include "request.h"
#include "arbiter.h"
#include <vector>
#include <deque>
#include <unordered_map>
#include <memory>

namespace npu_perf {

class Interconnect : public sc_module {
public:
    SC_HAS_PROCESS(Interconnect);
    Interconnect(sc_module_name n, NpuConfig c, uint32_t num_sources);

    // 供顶层绑定：第 i 个 DMA 绑到第 i 个 target socket。
    tlm_utils::simple_target_socket_tagged<Interconnect>& dma_socket(uint32_t i) {
        return *tsocks_[i];
    }
    // 下游：连到 MemoryController。
    tlm_utils::simple_initiator_socket<Interconnect>& mc_socket() { return isock_; }

    // 前向：来自某个 DMA（source 由 tagged socket 传入）。
    tlm_sync_enum nb_transport_fw(int source, tlm_generic_payload& gp,
                                  tlm_phase& phase, sc_time& delay);
    // 后向：来自 MemoryController。
    tlm_sync_enum nb_transport_bw(tlm_generic_payload& gp, tlm_phase& phase,
                                  sc_time& delay);

    // ---- 统计只读访问器 ----
    uint64_t requests()         const { return requests_; }            // 见过的请求总数
    uint64_t queue_full_events() const { return queue_full_events_; }  // 队列满次数
    uint64_t granted(uint32_t source) const;                           // 某源被授予次数（公平性）
    double   avg_queue_delay_ns()   const;   // 平均排队延迟 (ns)
    double   avg_total_latency_ns() const;   // 平均全链路延迟 (ns)
    double   total_stall_ns()       const;   // 背压总 stall 时间 (ns)

private:
    // 仲裁循环：有请求就挑一个转发，串行化互连带宽。
    void forward_loop();
    // 腾出队列槽后，把 blocked_ 队首请求 promote 进队列（解除背压）。
    void promote_blocked();

    NpuConfig cfg_;
    std::vector<std::unique_ptr<tlm_utils::simple_target_socket_tagged<Interconnect>>> tsocks_;
    tlm_utils::simple_initiator_socket<Interconnect> isock_;
    std::unique_ptr<Arbiter> arbiter_;

    std::deque<MemoryRequest*> queue_;     // 已接受、待仲裁转发（深度 ≤ queue_depth_）
    std::deque<MemoryRequest*> blocked_;   // 队列满时的背压等待（不占 queue 槽）
    // gp → MemoryRequest（响应路由 + 记账），非所有权（payload 归 DMA，request 归本类）。
    std::unordered_map<tlm_generic_payload*, MemoryRequest*> active_;

    sc_time  next_forward_free_ = SC_ZERO_TIME;   // 互连转发总线的下一次空闲时刻
    sc_event ev_queue_;                           // queue_ 由空转非空 → 唤醒仲裁循环

    uint64_t next_id_ = 0;
    uint64_t requests_ = 0;
    uint64_t queue_full_events_ = 0;
    uint64_t completed_ = 0;
    sc_time  total_queue_delay_ = SC_ZERO_TIME;   // Σ(转发时刻 - 到达时刻)
    sc_time  total_latency_     = SC_ZERO_TIME;   // Σ(完成时刻 - 到达时刻)
    sc_time  total_stall_       = SC_ZERO_TIME;   // Σ(解除背压时刻 - 到达时刻)
    std::unordered_map<uint32_t, uint64_t> granted_by_source_;  // 公平性统计
};

}  // namespace npu_perf
