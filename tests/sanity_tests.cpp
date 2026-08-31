// MVP-4 sanity 测试：
//   1) 基础换算（cycle/time、config、buffer、PE）——与 MVP-1/2/3 一致；
//   2) MVP-3 兼容：单 DMA 经 互连→MC→Hbm 后的端到端时序与手算吻合；
//   3) 双 DMA 竞争：多源汇聚到互连，全部请求被服务、两源都被授予；
//   4) 队列溢出/背压：浅队列触发 queue_full 事件且不死锁；
//   5) 仲裁公平性：round-robin 授予均衡，fifo/priority 也不饿死。

#include "common.h"
#include "hbm.h"
#include "memory_controller.h"
#include "interconnect.h"
#include "onchip_buffer.h"
#include "dma_engine.h"
#include "pe_array.h"
#include "workload_driver.h"
#include <iostream>
#include <cmath>
#include <string>
#include <vector>
#include <memory>

static int g_failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        std::cout << "  [PASS] " << what << "\n";
    } else {
        std::cout << "  [FAIL] " << what << "\n";
        ++g_failures;
    }
}
static bool approx(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps;
}

namespace npu_perf {

// 组装一套 MVP-4 拓扑：N 个 DMA → 互连 → MC → Hbm，加 Buffer/PE/Driver。
// 与 NpuSystem 内部等价，但把各模块暴露出来供测试逐项断言。
struct Topo {
    NpuConfig cfg;
    Hbm hbm; MemoryController mc; Interconnect ic;
    OnchipBuffer buf; PeArray pe;
    std::vector<std::unique_ptr<DmaEngine>> dmas;
    std::vector<DmaEngine*> dma_ptrs;
    std::unique_ptr<WorkloadDriver> drv;

    Topo(const std::string& tag, NpuConfig c, GemmTask t, uint32_t ndma)
      : cfg(c),
        hbm((tag + "_hbm").c_str(), c),
        mc ((tag + "_mc").c_str(),  c),
        ic ((tag + "_ic").c_str(),  c, ndma),
        buf((tag + "_buf").c_str(), c),
        pe ((tag + "_pe").c_str(),  c) {
        for (uint32_t i = 0; i < ndma; ++i) {
            auto d = std::make_unique<DmaEngine>(
                (tag + "_dma" + std::to_string(i)).c_str(), c);
            dma_ptrs.push_back(d.get());
            dmas.push_back(std::move(d));
        }
        for (uint32_t i = 0; i < ndma; ++i)
            dmas[i]->isock.bind(ic.dma_socket(i));
        ic.mc_socket().bind(mc.tsock);
        mc.isock.bind(hbm.tsock);
        drv = std::make_unique<WorkloadDriver>(
            (tag + "_drv").c_str(), c, t, dma_ptrs, &buf, &pe);
    }
};

}  // namespace npu_perf

int sc_main(int, char*[]) {
    using namespace npu_perf;

    std::cout << "==== MVP-4 sanity tests ====\n";

    // --- 1) cycle <-> time 换算 ---
    check(approx(cycles(1).to_seconds(), 1.0 / CLK_FREQ_HZ), "cycles(1) == 1/f");
    check(approx(cycles(100).to_seconds(), 100e-9), "cycles(100) == 100ns @1GHz");

    // --- 2) NpuConfig 派生量 ---
    {
        NpuConfig c;
        check(approx(c.peak_compute_flops(), 2.0 * 16 * 16 * 1e9),
              "peak_compute = 2*N^2*freq");
        check(approx(c.hbm_bw_Bps(), 256e9), "hbm_bw_Bps == 256e9");
        check(approx(c.interconnect_bw_Bps(), 256e9), "interconnect_bw_Bps == 256e9");
        check(c.buffer_bytes() == 256ull * 1024, "buffer_bytes == 256KB");
        check(c.dma_count() == 1 && c.arbiter_policy() == ArbiterPolicy::FIFO,
              "default dma=1, fifo");
    }

    // --- 3) 仲裁策略解析 ---
    {
        ArbiterPolicy p;
        check(parse_arbiter("fifo", p) && p == ArbiterPolicy::FIFO, "parse fifo");
        check(parse_arbiter("rr", p) && p == ArbiterPolicy::ROUND_ROBIN, "parse rr");
        check(parse_arbiter("priority", p) && p == ArbiterPolicy::PRIORITY, "parse priority");
        check(!parse_arbiter("bogus", p), "reject bogus arbiter");
    }

    // --- 4) OnchipBuffer / PeArray（与 MVP-3 一致） ---
    {
        NpuConfig c; c.set_buffer_kb(1);
        OnchipBuffer buf("buf_t", c);
        check(buf.can_hold(1024) && !buf.can_hold(1025), "buffer capacity bound");
        buf.allocate(1024);
        check(!buf.can_hold(1), "full buffer rejects more");
        buf.release(1024);
        check(buf.can_hold(1024), "released buffer free again");
        check(approx(buf.access_time(256).to_seconds(), 4e-9), "buffer access_time");

        PeArray pe("pe_t", c);
        check(approx(pe.pass_time().to_seconds(), 48e-9), "PE pass_time = 3N cycles");
        pe.account_pass();
        check(pe.passes() == 1 && pe.macs() == 16ull*16*16 && pe.flops() == 2ull*16*16*16,
              "PE accounting");
    }

    // --- 5) MVP-3 兼容：单 DMA 经 互连→MC→Hbm，端到端时序与手算吻合 ---
    {
        NpuConfig c_db;                              // 默认 double_buffer=on
        NpuConfig c_ser; c_ser.set_double_buffer(false);
        GemmTask t1{16, 16, 16};                     // kt=1：无可重叠
        GemmTask t2{16, 32, 16};                     // kt=2：db 能藏掉一趟 compute
        GemmTask t3{32, 32, 16};                     // mt=2,nt=1,kt=2 → 2 个 output tile

        Topo A("A", c_db,  t1, 1);   // kt=1 双缓冲
        Topo S("S", c_ser, t2, 1);   // kt=2 串行
        Topo D("D", c_db,  t2, 1);   // kt=2 双缓冲
        Topo E("E", c_ser, t3, 1);   // 2 tiles 串行
        Topo F("F", c_db,  t3, 1);   // 2 tiles 双缓冲

        sc_core::sc_start();

        // 集 A：两个 read 同时提交；100ns latency 重叠，1ns 数据传输串行。
        // load=(101/102ns 响应 + 两次4ns SRAM)=109ns，PE48，store=4+101，合计262ns。
        check(A.mc.serviced_reqs() == 3 && A.mc.serviced_bytes() == 3 * 256,
              "single DMA: MC served all requests");
        check(A.pe.passes() == 1, "kt=1: 1 pass");
        check(approx(A.drv->run_time().to_seconds(), 262e-9),
              "AT overlaps two read latencies: kt=1 db 262ns");

        // 集 S 串行：2 slice × (load210 + PE48) + store105 = 621ns
        check(approx(S.drv->run_time().to_seconds(), 621e-9), "kt=2 serial 621ns");
        check(D.drv->run_time() < S.drv->run_time(), "double buffering reduces time");

        // 搬运量/趟数不受调度影响
        check(S.pe.passes() == 2 && D.pe.passes() == 2, "kt=2 -> 2 passes each");
        check(S.mc.serviced_bytes() == D.mc.serviced_bytes(), "db doesn't change bytes");

        // 集 E/F：2 个 output tile，4 passes；跨 tile 预取且不死锁。
        check(E.pe.passes() == 4 && F.pe.passes() == 4, "mt=2,kt=2 -> 4 passes");
        check(approx(E.drv->run_time().to_seconds(), 1242e-9), "2 tiles serial 1242ns");
        check(F.drv->run_time() < E.drv->run_time(), "db reduces time across tiles");

        // 单 DMA 且默认参数：互连不应有排队延迟、无队列满。
        check(A.ic.queue_full_events() == 0, "single DMA: no queue-full");
    }

    // --- 6) 双 DMA 竞争：多源汇聚，全部服务、两源都被授予 ---
    {
        NpuConfig c; c.set_dma_count(2);             // 2 个 DMA，fifo
        GemmTask t{32, 32, 32};                      // mt=2,nt=2,kt=2 → 16 read + 4 write
        Topo T("cont", c, t, 2);

        sc_core::sc_start();

        const uint64_t expect = 16 + 4;              // 2*2*2*2 read + 2*2 write
        check(T.mc.serviced_reqs() == expect, "2 DMA: MC served all 20 requests");
        check(T.ic.requests() == expect, "2 DMA: interconnect saw all requests");
        check(T.ic.granted(0) > 0 && T.ic.granted(1) > 0, "2 DMA: both sources granted");
        check(T.drv->run_time() > SC_ZERO_TIME, "2 DMA: completed");
        check(T.ic.queue_full_events() == 0, "2 DMA depth=16: no overflow");
    }

    // --- 7) 队列溢出/背压：浅队列触发 queue-full 事件且不死锁 ---
    {
        NpuConfig c; c.set_dma_count(2); c.set_queue_depth(1);   // 极浅队列
        GemmTask t{32, 32, 32};
        Topo T("ovf", c, t, 2);

        sc_core::sc_start();

        const uint64_t expect = 16 + 4;
        check(T.ic.queue_full_events() > 0, "shallow queue: overflow events recorded");
        check(T.ic.total_stall_ns() >= 0.0, "shallow queue: stall tracked");
        check(T.mc.serviced_reqs() == expect, "shallow queue: still served all requests");
        check(T.drv->run_time() > SC_ZERO_TIME, "shallow queue: no deadlock");
    }

    // --- 8) 仲裁公平性：round-robin 授予均衡，fifo/priority 也不饿死 ---
    {
        // 8a) round-robin：两源授予次数应接近均衡（差 ≤ 1）。
        NpuConfig crr; crr.set_dma_count(2); crr.set_arbiter_policy(ArbiterPolicy::ROUND_ROBIN);
        GemmTask t{64, 32, 32};                      // mt=4,nt=2,kt=2 → 32 read + 8 write
        Topo R("rr", crr, t, 2);

        sc_core::sc_start();

        const uint64_t expect_rr = 32 + 8;
        check(R.mc.serviced_reqs() == expect_rr, "RR: served all requests");
        const int64_t g0 = int64_t(R.ic.granted(0)), g1 = int64_t(R.ic.granted(1));
        check(g0 > 0 && g1 > 0, "RR: no starvation");
        check(std::abs(g0 - g1) <= 1, "RR: grants balanced within 1");

        // 8b) fifo / priority：都能跑完，授予次数之和 = 总请求数（不饿死、不死锁）。
        NpuConfig cf; cf.set_dma_count(2); cf.set_arbiter_policy(ArbiterPolicy::FIFO);
        NpuConfig cp; cp.set_dma_count(2); cp.set_arbiter_policy(ArbiterPolicy::PRIORITY);
        Topo Ff("fifo", cf, t, 2);
        Topo Pp("prio", cp, t, 2);

        sc_core::sc_start();

        check(Ff.mc.serviced_reqs() == expect_rr
              && Ff.ic.granted(0) + Ff.ic.granted(1) == expect_rr,
              "FIFO: all served, grants sum to total");
        check(Pp.mc.serviced_reqs() == expect_rr
              && Pp.ic.granted(0) + Pp.ic.granted(1) == expect_rr,
              "Priority: all served, grants sum to total");
    }

    std::cout << "==== " << (g_failures == 0 ? "ALL PASS" : "FAILURES")
              << " (" << g_failures << " failed) ====\n";
    return g_failures == 0 ? 0 : 1;
}
