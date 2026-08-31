#pragma once
// ============================================================================
// MemoryRequest —— MVP-4 的请求模型（Development order Step 1）。
//
// 一次访存事务从诞生到结束的完整生命周期：状态机 + 三处关键时间戳。
// 注意它是"性能记账"对象，与真正的 TLM payload（tlm_generic_payload）是两回事：
//   - tlm_generic_payload 由 DMA 持有到 END_RESP，负责在 socket 间搬运；
//   - MemoryRequest 由 Interconnect 创建/持有，负责记录调度与排队时序。
// 两者通过 gp 指针关联（Interconnect 维护 gp → MemoryRequest 的映射）。
//
// 状态机（对应 MVP4-Detailed-Design-Specification §4）：
//     CREATED ─► QUEUED ─► GRANTED ─► SERVING ─► COMPLETED
//     （DMA 发出）（入队）（仲裁选中）（转发下游）（响应返回）
//
// 时间戳（用于统计 latency / queueing / fairness）：
//   issue_time   请求到达 Interconnect 的时刻（CREATED）
//   grant_time   仲裁器选中并真正转发出去的时刻（GRANTED）
//   finish_time  响应 BEGIN_RESP 送回源 DMA 的时刻（COMPLETED）
// ============================================================================

#include "common.h"

namespace npu_perf {

class MemoryRequest {
public:
    enum State { CREATED, QUEUED, GRANTED, SERVING, COMPLETED };

    MemoryRequest(uint64_t id, uint64_t addr, uint32_t size, uint32_t source,
                  tlm_generic_payload* gp)
        : id_(id), address_(addr), size_(size), source_(source), gp_(gp),
          issue_time_(sc_time_stamp()) {}

    // ---- 只读访问器 ----
    uint64_t id()      const { return id_; }
    uint64_t address() const { return address_; }
    uint32_t size()    const { return size_; }
    uint32_t source()  const { return source_; }        // 哪个 DMA 发的
    tlm_generic_payload* gp() const { return gp_; }
    State    state()   const { return state_; }

    sc_time issue_time()  const { return issue_time_; }
    sc_time grant_time()  const { return grant_time_; }
    sc_time finish_time() const { return finish_time_; }

    // ---- 派生计时指标 ----
    sc_time queue_delay()   const { return grant_time_ - issue_time_; }   // 排队等待仲裁+带宽
    sc_time service_latency() const { return finish_time_ - grant_time_; } // 下游服务时间
    sc_time total_latency() const { return finish_time_ - issue_time_; }   // 全链路

    // ---- 生命周期推进（仅 Interconnect 调用）----
    void set_state(State s)           { state_ = s; }
    void set_grant_time(sc_time t)    { grant_time_ = t; }
    void set_finish_time(sc_time t)   { finish_time_ = t; }

private:
    uint64_t id_;
    uint64_t address_;
    uint32_t size_;
    uint32_t source_;
    tlm_generic_payload* gp_;         // 非所有权：payload 归 DMA，这里只做关联
    State  state_ = CREATED;
    sc_time issue_time_;
    sc_time grant_time_  = SC_ZERO_TIME;
    sc_time finish_time_ = SC_ZERO_TIME;
};

}  // namespace npu_perf
