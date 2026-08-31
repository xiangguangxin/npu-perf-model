// Interconnect 的实现：队列 + 仲裁 + 背压 + 网络延迟的共享互连。
#include "interconnect.h"
#include <algorithm>

namespace npu_perf {

Interconnect::Interconnect(sc_module_name n, NpuConfig c, uint32_t num_sources)
  : sc_module(n), cfg_(c), isock_("mc_socket"),
    arbiter_(make_arbiter(c.arbiter_policy(), num_sources)) {
    // 每个 DMA 一个 tagged target socket，用 id 打标以便回调知道来源。
    for (uint32_t i = 0; i < num_sources; ++i) {
        auto name = std::string("dma_socket") + std::to_string(i);
        tsocks_.emplace_back(
            std::make_unique<tlm_utils::simple_target_socket_tagged<Interconnect>>(
                name.c_str()));
        tsocks_[i]->register_nb_transport_fw(this, &Interconnect::nb_transport_fw, int(i));
    }
    isock_.register_nb_transport_bw(this, &Interconnect::nb_transport_bw);
    SC_THREAD(forward_loop);
}

// ---- 前向路径：来自某个 DMA ----
tlm_sync_enum Interconnect::nb_transport_fw(int source, tlm_generic_payload& gp,
                                            tlm_phase& phase, sc_time& delay) {
    if (phase == END_RESP) {
        // DMA 收到 BEGIN_RESP 后回 END_RESP：转发给 MC 闭合协议（MC → Hbm）。
        tlm_phase p = END_RESP;
        isock_->nb_transport_fw(gp, p, delay);
        return TLM_COMPLETED;
    }
    if (phase != BEGIN_REQ) {
        SC_REPORT_ERROR("Interconnect", "unexpected forward phase");
        return TLM_COMPLETED;
    }

    // 建立请求记账对象（response 阶段据此路由回源 + 汇总时序）。
    auto* r = new MemoryRequest(next_id_++, gp.get_address(),
                                gp.get_data_length(), source, &gp);
    active_[&gp] = r;
    ++requests_;

    if (queue_.size() < cfg_.queue_depth()) {
        // 有槽：接受，立刻回 END_REQ，入队等待仲裁。
        queue_.push_back(r);
        r->set_state(MemoryRequest::QUEUED);
        tlm_phase p = END_REQ; sc_time z = SC_ZERO_TIME;
        (*tsocks_[source])->nb_transport_bw(gp, p, z);
        ev_queue_.notify(SC_ZERO_TIME);
    } else {
        // 队列满：背压 —— 暂不回 END_REQ、不入队，等有槽再 promote。
        blocked_.push_back(r);
        ++queue_full_events_;
    }
    return TLM_ACCEPTED;
}

// ---- 后向路径：来自 MemoryController ----
tlm_sync_enum Interconnect::nb_transport_bw(tlm_generic_payload& gp,
                                            tlm_phase& phase, sc_time& delay) {
    auto it = active_.find(&gp);
    if (it == active_.end()) {
        SC_REPORT_ERROR("Interconnect", "backward phase for unknown transaction");
        return TLM_COMPLETED;
    }
    MemoryRequest* r = it->second;
    const uint32_t src = r->source();

    if (phase == END_REQ) {
        // 下游（MC/Hbm）的 END_REQ 在此"吞掉"：本互连在接收请求入队（或背压
        // 解除 promote）时，已经向上游 DMA 发过自己的 END_REQ，请求通道早已
        // 释放。四相协议里每笔事务只应收到一次 END_REQ，这里不再二次转发，
        // 否则 DMA 会对同一笔事务收到两个 END_REQ（协议不一致）。
        return TLM_ACCEPTED;
    }

    if (phase == BEGIN_RESP) {
        // 请求走完全程：记录完成时刻，汇总统计，再把响应送回源 DMA。
        r->set_finish_time(sc_time_stamp());
        r->set_state(MemoryRequest::COMPLETED);
        ++completed_;
        total_latency_ += r->total_latency();

        tlm_phase p = BEGIN_RESP; sc_time z = SC_ZERO_TIME;
        // 该调用会同步触发源 DMA 回 END_RESP → 本类的 nb_transport_fw(END_RESP)。
        (*tsocks_[src])->nb_transport_bw(gp, p, z);

        // 事务闭合：释放记账对象（payload 仍归 DMA，到 END_RESP 结束）。
        active_.erase(it);
        delete r;
        return TLM_COMPLETED;
    }

    SC_REPORT_ERROR("Interconnect", "unexpected backward phase");
    return TLM_COMPLETED;
}

// ---- 仲裁循环：挑请求、串行化互连带宽、转发下游 ----
void Interconnect::forward_loop() {
    while (true) {
        while (queue_.empty()) wait(ev_queue_);

        // 仲裁：挑一个请求（FIFO/RR/Priority）。
        MemoryRequest* r = arbiter_->select(queue_);
        queue_.erase(std::find(queue_.begin(), queue_.end(), r));
        r->set_state(MemoryRequest::GRANTED);
        granted_by_source_[r->source()] += 1;

        // 互连转发总线串行化：等上一次转发结束（next_forward_free_）。
        if (next_forward_free_ > sc_time_stamp())
            wait(next_forward_free_ - sc_time_stamp());
        r->set_grant_time(sc_time_stamp());
        total_queue_delay_ += r->queue_delay();   // 排队等待仲裁+带宽的时间

        // 占用总线到 bytes/interconnect_bw 之后，才能转发下一笔。
        next_forward_free_ = sc_time_stamp()
                           + sc_time(double(r->size()) / cfg_.interconnect_bw_Bps(), SC_SEC);
        r->set_state(MemoryRequest::SERVING);

        // 转发 BEGIN_REQ 给 MC，带 noc_latency 单跳延迟。
        tlm_phase ph = BEGIN_REQ;
        sc_time   d  = cycles(cfg_.noc_latency());
        isock_->nb_transport_fw(*r->gp(), ph, d);

        // 刚腾出一个队列槽：若此前有被背压阻塞的请求，promote 一个。
        promote_blocked();
    }
}

// 队列有空槽时，把最早被阻塞的请求转正：回 END_REQ、入队、记 stall。
void Interconnect::promote_blocked() {
    if (blocked_.empty()) return;
    if (queue_.size() >= cfg_.queue_depth()) return;

    MemoryRequest* r = blocked_.front();
    blocked_.pop_front();
    queue_.push_back(r);
    r->set_state(MemoryRequest::QUEUED);
    total_stall_ += sc_time_stamp() - r->issue_time();

    tlm_phase p = END_REQ; sc_time z = SC_ZERO_TIME;
    (*tsocks_[r->source()])->nb_transport_bw(*r->gp(), p, z);
    ev_queue_.notify(SC_ZERO_TIME);
}

// ---- 统计访问器 ----
uint64_t Interconnect::granted(uint32_t source) const {
    auto it = granted_by_source_.find(source);
    return it == granted_by_source_.end() ? 0 : it->second;
}

double Interconnect::avg_queue_delay_ns() const {
    return requests_ ? total_queue_delay_.to_seconds() * 1e9 / double(requests_) : 0.0;
}

double Interconnect::avg_total_latency_ns() const {
    return completed_ ? total_latency_.to_seconds() * 1e9 / double(completed_) : 0.0;
}

double Interconnect::total_stall_ns() const {
    return total_stall_.to_seconds() * 1e9;
}

}  // namespace npu_perf
