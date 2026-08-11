#pragma once
// ============================================================================
// DmaEngine —— MVP-3 的 TLM-2.0 AT initiator（只建时序，不搬真实数据）。
//
// 【MVP-3 框架图：发起端的职责与对象生命周期】
//
// WorkloadDriver (loader / serial thread)
//   │ issue_read() / read()
//   ▼
// DmaEngine
//   ├─ Transfer：一次事务独占的 gp + TileExtension + done_ev，必须活到 END_RESP
//   ├─ active_：用 gp 地址找到等待响应的 Transfer
//   ├─ outstanding_：在途计数；达到 cfg.dma_outstanding() 时暂停发起方
//   └─ isock ── nb_transport_fw / nb_transport_bw ──> Memory::tsock
//
// 【四相协议（时间从左到右）】
//   DMA  -- BEGIN_REQ -->  Memory    请求已提交，payload 仍归 DMA 所有
//   DMA  <-- END_REQ ---   Memory    target 接收完成，可继续发下一个请求
//   DMA  <-- BEGIN_RESP -- Memory    HBM 延迟和带宽时间已过去，结果可消费
//   DMA  -- END_RESP -->   Memory    事务收尾；DMA 才释放 Transfer/outstanding 槽
//
// 本模型允许最多 dma_outstanding 个请求同时处于上述四相流程中，用并发请求
// 隐藏 HBM 固定延迟；Memory 侧仍会将数据传输阶段串行化，故不会虚增带宽。
// ============================================================================

#include "common.h"
#include <memory>
#include <unordered_map>

class DmaEngine : public sc_module {
public:
    tlm_utils::simple_initiator_socket<DmaEngine> isock;   // 对外可绑定，故留 public

    DmaEngine(sc_module_name n, NpuConfig c)
      : sc_module(n), isock("isock"), cfg_(c) {
        isock.register_nb_transport_bw(this, &DmaEngine::nb_transport_bw);
      }

    class Transfer {
    public:
        bool complete() const { return complete_; }

    private:
        friend class DmaEngine;
        // payload 与 extension 都是事务私有对象：AT target 在异步响应前持有 gp_ 指针，
        // 因而绝不能像 LT 实现那样放在 submitTransfer() 的栈帧中。
        tlm_generic_payload gp_;
        TileExtension ext_;
        sc_event done_ev_;            // BEGIN_RESP/END_RESP 完成时唤醒 wait_for()
        bool complete_ = false;       // 防止零延迟通知先于 wait() 而造成漏唤醒
    };
    using TransferPtr = std::shared_ptr<Transfer>;

    // ---- 同步外观 API：内部仍走完整 AT 协议，随后等待该请求完成 ----
    // HBM -> 片上：搬一块 tile 进来（用于 load 权重/激活）
    void read(uint64_t addr, uint32_t bytes, TileExtension::Kind kind,
              uint32_t tile_id = 0);
    // 片上 -> HBM：把一块 output tile 写回
    void write(uint64_t addr, uint32_t bytes, TileExtension::Kind kind,
               uint32_t tile_id = 0);

    // ---- 异步 API：立即提交请求，调用方可先提交其他事务再 wait_for() ----
    // 典型用法：auto w=issue_read(...); auto a=issue_read(...); wait_for(w); wait_for(a);
    TransferPtr issue_read(uint32_t bytes, TileExtension::Kind kind, uint32_t tile_id = 0);
    TransferPtr issue_write(uint32_t bytes, TileExtension::Kind kind, uint32_t tile_id = 0);
    void wait_for(const TransferPtr& transfer);

    // ---- 统计只读访问器 ----
    uint64_t bytes_moved() const { return bytes_moved_; }   // 累计搬运字节
    uint64_t num_xfers()   const { return num_xfers_; }     // 搬运次数

private:
    // 公共 read/write 与 issue_* 最终都会进入此处；这里是 BEGIN_REQ 发出的唯一位置。
    TransferPtr submitTransfer(tlm_command cmd, uint64_t addr, uint32_t bytes,
                      TileExtension::Kind kind, uint32_t tile_id);
    // Memory 通过 backward path 回调此函数。END_REQ 只确认接收；BEGIN_RESP 负责回
    // END_RESP，并释放本 DMA 的 in-flight slot。
    tlm_sync_enum nb_transport_bw(tlm_generic_payload& gp, tlm_phase& phase,
                                  sc_time& delay);
    // 统一收尾路径，兼容异步 BEGIN_RESP 和少数 target 可能返回的立即完成情形。
    void complete(Transfer& transfer);

    NpuConfig cfg_;
    uint64_t bytes_moved_ = 0;
    uint64_t num_xfers_   = 0;
    uint32_t outstanding_ = 0;             // 已 BEGIN_REQ、尚未 END_RESP 的事务数
    sc_event slot_free_ev_;                // complete() 后唤醒被额度限制的发起方
    std::unordered_map<tlm_generic_payload*, Transfer*> active_; // 回调关联表（非所有权）
};
