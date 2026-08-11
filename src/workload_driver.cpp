// WorkloadDriver 的实现：GEMM 按 output tile 分块，串行 / 双缓冲两种调度。
// 设计框图与同步语义见 workload_driver.h 头部注释。
#include "workload_driver.h"

WorkloadDriver::WorkloadDriver(sc_module_name n, NpuConfig c, GemmTask t,
                               DmaEngine* d, OnchipBuffer* b, PeArray* p)
  : sc_module(n), cfg_(c), task_(t), dma_(d), buf_(b), pe_(p) {
    // 按调度模式注册对应的进程：双缓冲=loader+compute 两线程；串行=单线程。
    if (cfg_.double_buffer()) { SC_THREAD(loader); SC_THREAD(compute); }
    else                      { SC_THREAD(run_serial); }
}

// load 一块 tile：HBM -> 片上（HBM 延迟+带宽，再加写进 buffer 的带宽时间）
void WorkloadDriver::load_tile(TileExtension::Kind kind, uint32_t bytes, uint32_t tid) {
    dma_->read(/*addr=*/0, bytes, kind, tid);   // HBM -> 片上
    wait(buf_->access_time(bytes));             // 写进 buffer 的带宽时间
}

// store 一块 output tile：片上 -> HBM（先从 buffer 读出，再经 DMA 写回）
void WorkloadDriver::store_tile(uint32_t bytes, uint32_t tid) {
    wait(buf_->access_time(bytes));                        // 从 buffer 读出
    dma_->write(/*addr=*/0, bytes, TileExtension::OUTPUT, tid);
}

// ================= 串行调度：搬和算完全不重叠（对照下界）=================
void WorkloadDriver::run_serial() {
    const sc_time t0 = sc_time_stamp();
    const uint32_t n = cfg_.array_n();
    const uint32_t bytes = tile_bytes();
    const uint32_t mt = ceil_div(task_.M(), n);
    const uint32_t nt = ceil_div(task_.N(), n);
    const uint32_t kt = ceil_div(task_.K(), n);

    uint32_t tid = 0;
    for (uint32_t i = 0; i < mt; ++i) {
        for (uint32_t j = 0; j < nt; ++j) {
            buf_->allocate(bytes);                       // output tile 累加器
            for (uint32_t k = 0; k < kt; ++k) {
                load_tile(TileExtension::WEIGHT,     bytes, tid);
                load_tile(TileExtension::ACTIVATION, bytes, tid);
                wait(pe_->pass_time());                  // PE 算一趟
                pe_->account_pass();
                ++tid;
            }
            store_tile(bytes, tid++);
            buf_->release(bytes);
        }
    }
    run_time_ = sc_time_stamp() - t0;
}

// ================= 双缓冲调度：loader / compute 两线程 ping-pong =================
// 生产者：按 (i,j,k) 顺序把每个 K-slice 载入一个空槽。
void WorkloadDriver::loader() {
    const uint32_t n = cfg_.array_n();
    const uint32_t bytes = tile_bytes();
    const uint32_t mt = ceil_div(task_.M(), n);
    const uint32_t nt = ceil_div(task_.N(), n);
    const uint32_t kt = ceil_div(task_.K(), n);

    uint32_t tid = 0;
    for (uint32_t i = 0; i < mt; ++i)
    for (uint32_t j = 0; j < nt; ++j)
    for (uint32_t k = 0; k < kt; ++k) {
        while (free_slots_ == 0) wait(ev_free_);     // 双缓冲满 → 等 compute 腾槽
        --free_slots_;
        buf_->allocate(bytes);
        // 两个操作数同一时刻提交，AT DMA 可让固定 HBM 延迟重叠。
        auto weight = dma_->issue_read(bytes, TileExtension::WEIGHT, tid);
        auto activation = dma_->issue_read(bytes, TileExtension::ACTIVATION, tid);
        dma_->wait_for(weight);
        wait(buf_->access_time(bytes));
        dma_->wait_for(activation);
        wait(buf_->access_time(bytes));
        ++filled_slots_;
        ev_filled_.notify(SC_ZERO_TIME);             // 通知 compute：有满槽
        ++tid;
    }
}

// 消费者：按同样顺序取满槽做 PE 计算；每个 output tile 的 K 累加完负责写回。
void WorkloadDriver::compute() {
    const sc_time t0 = sc_time_stamp();
    const uint32_t n = cfg_.array_n();
    const uint32_t bytes = tile_bytes();
    const uint32_t mt = ceil_div(task_.M(), n);
    const uint32_t nt = ceil_div(task_.N(), n);
    const uint32_t kt = ceil_div(task_.K(), n);

    uint32_t tid = 0;
    for (uint32_t i = 0; i < mt; ++i) {
        for (uint32_t j = 0; j < nt; ++j) {
            for (uint32_t k = 0; k < kt; ++k) {
                while (filled_slots_ == 0) wait(ev_filled_);  // 等 loader 载好
                --filled_slots_;
                wait(pe_->pass_time());              // PE 算一趟
                pe_->account_pass();
                buf_->release(bytes);                // 释放该 slot
                ++free_slots_;
                ev_free_.notify(SC_ZERO_TIME);       // 通知 loader：腾出空槽
                ++tid;
            }
            store_tile(bytes, tid++);                // 该 output tile 写回
        }
    }
    run_time_ = sc_time_stamp() - t0;                // compute 最后结束，由它记时
}
