// MVP-1 sanity 测试：验证 timing 换算、Memory 延迟+带宽建模、Buffer 约束，
// 以及端到端数据流的仿真时间与手算是否吻合（perf 模型可信度的第一道关）。

#include "common.h"
#include "memory.h"
#include "onchip_buffer.h"
#include "dma_engine.h"
#include "pe_array.h"
#include "workload_driver.h"
#include <iostream>
#include <cmath>

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

int sc_main(int, char*[]) {
    using namespace npu_perf;

    std::cout << "==== MVP-3 sanity tests ====\n";

    // --- 1) cycle <-> time 换算 ---
    check(approx(cycles(1).to_seconds(), 1.0 / CLK_FREQ_HZ), "cycles(1) == 1/f");
    check(approx(cycles(100).to_seconds(), 100e-9), "cycles(100) == 100ns @1GHz");

    // --- 2) NpuConfig 派生量 ---
    {
        NpuConfig c;                       // array_n=16, hbm_bw=256GB/s
        check(approx(c.peak_compute_flops(), 2.0 * 16 * 16 * 1e9),
              "peak_compute = 2*N^2*freq");
        check(approx(c.hbm_bw_Bps(), 256e9), "hbm_bw_Bps == 256e9");
        check(c.buffer_bytes() == 256ull * 1024, "buffer_bytes == 256KB");
    }

    // --- 3) OnchipBuffer：容量约束 + 访问时间 ---
    {
        NpuConfig c; c.set_buffer_kb(1);              // 1KB = 1024B
        OnchipBuffer buf("buf_t", c);
        check(buf.can_hold(1024), "buffer holds exactly capacity");
        check(!buf.can_hold(1025), "buffer rejects over-capacity");
        buf.allocate(1024);
        check(!buf.can_hold(1), "full buffer rejects more");
        buf.release(1024);
        check(buf.can_hold(1024), "released buffer free again");
        // access_time(256) = ceil(256/64)=4 cycle = 4ns
        check(approx(buf.access_time(256).to_seconds(), 4e-9),
              "buffer access_time = ceil(bytes/bw) cycles");
    }

    // --- 4) PeArray：一趟时间 + MAC 记账 ---
    {
        NpuConfig c;                        // array_n=16
        PeArray pe("pe_t", c);
        // pass_time = fill+steady+drain = 3*16 = 48 cycle = 48ns
        check(approx(pe.pass_time().to_seconds(), 48e-9), "PE pass_time = 3N cycles");
        pe.account_pass();
        check(pe.passes() == 1, "PE counted 1 pass");
        check(pe.macs() == 16ull * 16 * 16, "PE MACs = N^3 per pass");
        check(pe.flops() == 2ull * 16 * 16 * 16, "PE FLOP = 2*MAC");
    }

    // --- 5) 端到端 GEMM(搬+算)：AT 四相路径 + 双缓冲 ---
    //   run() 不再调 sc_stop()，多个 driver 用各自的模块实例互不干扰、各测各的时间。
    {
        NpuConfig c_db;                          // 默认 double_buffer=on
        NpuConfig c_ser; c_ser.set_double_buffer(false);
        GemmTask  t1{16, 16, 16};                // kt=1：无可重叠，db==serial
        GemmTask  t2{16, 32, 16};                // kt=2：db 能藏掉一趟 compute

        // 集 A：kt=1，双缓冲
        Memory mA("mA", c_db); OnchipBuffer bA("bA", c_db);
        DmaEngine dA("dA", c_db); PeArray pA("pA", c_db);
        WorkloadDriver drvA("drvA", c_db, t1, &dA, &bA, &pA);
        dA.isock.bind(mA.tsock);

        // 集 S：kt=2，串行
        Memory mS("mS", c_ser); OnchipBuffer bS("bS", c_ser);
        DmaEngine dS("dS", c_ser); PeArray pS("pS", c_ser);
        WorkloadDriver drvS("drvS", c_ser, t2, &dS, &bS, &pS);
        dS.isock.bind(mS.tsock);

        // 集 D：kt=2，双缓冲
        Memory mD("mD", c_db); OnchipBuffer bD("bD", c_db);
        DmaEngine dD("dD", c_db); PeArray pD("pD", c_db);
        WorkloadDriver drvD("drvD", c_db, t2, &dD, &bD, &pD);
        dD.isock.bind(mD.tsock);

        // 多 output tile（mt=2, kt=2）：验证两线程跨 tile 预取、不死锁
        GemmTask  t3{32, 32, 16};                // mt=2, nt=1, kt=2 → 2 个 output tile
        Memory mE("mE", c_ser); OnchipBuffer bE("bE", c_ser);
        DmaEngine dE("dE", c_ser); PeArray pE("pE", c_ser);
        WorkloadDriver drvE("drvE", c_ser, t3, &dE, &bE, &pE);   // 串行
        dE.isock.bind(mE.tsock);
        Memory mF("mF", c_db); OnchipBuffer bF("bF", c_db);
        DmaEngine dF("dF", c_db); PeArray pF("pF", c_db);
        WorkloadDriver drvF("drvF", c_db, t3, &dF, &bF, &pF);    // 双缓冲
        dF.isock.bind(mF.tsock);

        sc_core::sc_start();

        // 集 A：两个 read 同时提交；100ns latency 重叠，1ns 数据传输串行。
        // load=(101/102ns 响应 + 两次4ns SRAM) =109ns，PE48，store=4+101，合计262ns。
        check(dA.num_xfers() == 3, "kt=1: 3 xfers");
        check(mA.serviced_reqs() == 3 && mA.serviced_bytes() == 3 * 256,
              "AT Memory serviced all requests");
        check(pA.passes() == 1, "kt=1: 1 pass");
        check(approx(drvA.run_time().to_seconds(), 262e-9),
              "AT overlaps two read latencies: kt=1 db 262ns");

        // 集 S 串行：2 slice × (load210 + PE48) + store105 = 621ns
        check(approx(drvS.run_time().to_seconds(), 621e-9), "kt=2 serial 621ns");
        // AT 双缓冲不仅能重叠搬算，也能让同一 tile 的 weight/activation 延迟重叠。
        check(drvD.run_time() < drvS.run_time(), "double buffering reduces time");

        // 搬运量/趟数不受调度影响：db 只重叠、不改变流量
        check(pS.passes() == 2 && pD.passes() == 2, "kt=2 -> 2 passes each");
        check(dS.bytes_moved() == dD.bytes_moved(), "db doesn't change bytes moved");

        // 集 E/F：2 个 output tile。AT 双缓冲可跨 tile 预取且不死锁。
        check(pF.passes() == 4 && pE.passes() == 4, "mt=2,kt=2 -> 4 passes");
        check(dE.bytes_moved() == dF.bytes_moved(), "db doesn't change bytes (2 tiles)");
        check(approx(drvE.run_time().to_seconds(), 1242e-9), "2 tiles serial 1242ns");
        check(drvF.run_time() < drvE.run_time(), "db reduces time across tiles");
    }

    std::cout << "==== " << (g_failures == 0 ? "ALL PASS" : "FAILURES")
              << " (" << g_failures << " failed) ====\n";
    return g_failures == 0 ? 0 : 1;
}
