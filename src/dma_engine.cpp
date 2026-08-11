// DmaEngine 的实现：AT initiator，负责四相握手和在途请求额度。
#include "dma_engine.h"

// 同步外观：AT 请求仍先被提交，随后该调用线程只等待“本请求”的完成事件。
// 因此 API 适合串行调度；需要让多个请求共享 latency 时，应使用 issue_* + wait_for。
void DmaEngine::read(uint64_t addr, uint32_t bytes, TileExtension::Kind kind,
                     uint32_t tile_id) {
    wait_for(submitTransfer(TLM_READ_COMMAND, addr, bytes, kind, tile_id));
}

void DmaEngine::write(uint64_t addr, uint32_t bytes, TileExtension::Kind kind,
                      uint32_t tile_id) {
    wait_for(submitTransfer(TLM_WRITE_COMMAND, addr, bytes, kind, tile_id));
}

// 非阻塞提交：返回的 shared_ptr 同时保证 payload 生命周期跨越异步的 backward callback。
DmaEngine::TransferPtr DmaEngine::issue_read(uint32_t bytes, TileExtension::Kind kind, uint32_t tile_id) {
    return submitTransfer(TLM_READ_COMMAND, /*addr=*/0, bytes, kind, tile_id);
}

DmaEngine::TransferPtr DmaEngine::issue_write(uint32_t bytes, TileExtension::Kind kind, uint32_t tile_id) {
    return submitTransfer(TLM_WRITE_COMMAND, /*addr=*/0, bytes, kind, tile_id);
}

void DmaEngine::wait_for(const TransferPtr& transfer) {
    // 使用 while 而非单次 wait：若 BEGIN_RESP 已在本 delta cycle 更早到达，complete_
    // 会让调用者直接返回；若尚未到达则等待 done_ev_，避免丢失零延迟 notify。
    while (!transfer->complete_) wait(transfer->done_ev_);
}

DmaEngine::TransferPtr DmaEngine::submitTransfer(tlm_command cmd, uint64_t addr, uint32_t bytes,
                                                  TileExtension::Kind kind, uint32_t tile_id) {
    // outstanding 限制的是“已发 BEGIN_REQ 但尚未完成 END_RESP”的总数，不是仅在
    // Memory 队列里的数量。这样可同时约束 payload 占用和 DMA 本身的请求窗口。
    while (outstanding_ >= cfg_.dma_outstanding()) wait(slot_free_ev_);
    auto transfer = std::make_shared<Transfer>();
    // 这是 timing-only 模型；Memory 不解引用 data_ptr。仍设置哑指针以满足 generic
    // payload 的基本约定。真实功能模型则应分配至少 bytes 长度的数据缓冲。
    static unsigned char dummy[1] = {0};

    transfer->ext_.set_kind(kind);
    transfer->ext_.set_tile_id(tile_id);
    auto& gp = transfer->gp_;
    gp.set_command(cmd);
    gp.set_address(addr);
    gp.set_data_ptr(dummy);
    gp.set_data_length(bytes);
    gp.set_streaming_width(bytes);
    gp.set_byte_enable_ptr(nullptr);
    gp.set_dmi_allowed(false);
    gp.set_response_status(TLM_INCOMPLETE_RESPONSE);
    gp.set_extension(&transfer->ext_);

    sc_time delay = SC_ZERO_TIME;
    tlm_phase phase = BEGIN_REQ;
    // 先登记再调用 target：合法 target 可能同步返回 TLM_COMPLETED 或 TLM_UPDATED，
    // 两种路径都需要能通过 gp 地址找到这笔事务。
    active_[&gp] = transfer.get();
    ++outstanding_;
    const tlm_sync_enum status = isock->nb_transport_fw(gp, phase, delay);
    if (status == TLM_COMPLETED) {
        // target 在 fw 调用内已经结束事务（本项目的 Memory 不使用该快捷路径）。
        complete(*transfer);
    } else if (status == TLM_UPDATED && phase == BEGIN_RESP) {
        // 兼容 target 在 fw 返回值中立即给 BEGIN_RESP 的合法快捷路径。
        tlm_phase end_resp = END_RESP;
        sc_time end_delay = delay;
        isock->nb_transport_fw(gp, end_resp, end_delay);
        complete(*transfer);
    }

    bytes_moved_ += bytes;
    num_xfers_   += 1;
    return transfer;
}

// TLM 2.0 后向路径回调：处理 target（Memory）返回的四相握手响应。
// 四相协议流程：BEGIN_REQ(fw) → END_REQ(bw) → BEGIN_RESP(bw) → END_RESP(fw)
tlm_sync_enum DmaEngine::nb_transport_bw(tlm_generic_payload& gp, tlm_phase& phase,
                                         sc_time& delay) {
    // 1. 根据 gp 地址查找对应的事务
    auto it = active_.find(&gp);
    if (it == active_.end()) {
        SC_REPORT_ERROR("DmaEngine", "response for an unknown transaction");
        return TLM_COMPLETED;
    }
    Transfer& transfer = *it->second;

    // 2. 第二拍 END_REQ：target 已接收请求，只需确认即可
    if (phase == END_REQ) {
        return TLM_ACCEPTED;
    }

    // 3. 第三拍 BEGIN_RESP：target 返回响应，立即发出第四拍 END_RESP 闭合协议
    if (phase == BEGIN_RESP) {
        gp.set_response_status(TLM_OK_RESPONSE);
        tlm_phase end_resp = END_RESP;
        const tlm_sync_enum status = isock->nb_transport_fw(gp, end_resp, delay);
        if (status != TLM_COMPLETED) {
            SC_REPORT_ERROR("DmaEngine", "target did not complete END_RESP");
        }
        complete(transfer);   // 事务完成，释放槽位
        return TLM_COMPLETED;
    }

    // 4. 不应出现其他 phase
    SC_REPORT_ERROR("DmaEngine", "unexpected backward phase");
    return TLM_COMPLETED;
}

void DmaEngine::complete(Transfer& transfer) {
    if (transfer.complete_) return;
    if (transfer.gp_.get_response_status() != TLM_OK_RESPONSE) {
        SC_REPORT_ERROR("DmaEngine", "target returned error response");
    }
    // 从关联表移除后，任何重复响应都会被报告为 unknown transaction；清除 extension
    // 避免 tlm_generic_payload 析构时将 Transfer 成员 ext_ 当作堆对象释放。
    active_.erase(&transfer.gp_);
    transfer.gp_.clear_extension(&transfer.ext_);
    transfer.complete_ = true;
    --outstanding_; // 只在 END_RESP 闭合后回收 DMA 请求窗口
    transfer.done_ev_.notify(SC_ZERO_TIME);
    slot_free_ev_.notify(SC_ZERO_TIME);
}
