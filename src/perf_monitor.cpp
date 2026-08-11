// PerfMonitor 的实现：仿真结束后汇总访存/计算侧指标，打印报告 + CSV 一行。
#include "perf_monitor.h"
#include <iostream>
#include <iomanip>

// 理想最小搬运字节（无复用惩罚）
double PerfMonitor::bytes_min(const NpuConfig& c, const GemmTask& t) {
    return (double(t.M()) * t.K() + double(t.K()) * t.N() + double(t.M()) * t.N())
         * c.data_bytes();
}

void PerfMonitor::report(const NpuConfig& cfg, const GemmTask& t,
                         const Memory& mem, const DmaEngine& dma,
                         const PeArray& pe, const WorkloadDriver& drv) {
    const double sim_s      = drv.run_time().to_seconds();
    const double moved      = double(dma.bytes_moved());
    const double bw_ach_GBps = sim_s > 0 ? moved / sim_s / 1e9 : 0.0;

    // 解析下界：纯带宽时间（不含每块固定延迟）
    const double t_mem_bw   = moved / cfg.hbm_bw_Bps();
    // 串行基线：每笔事务都支付固定延迟；AT outstanding 可隐藏其中一部分。
    const double t_lat_all  = double(dma.num_xfers()) * cfg.hbm_lat_cyc() / CLK_FREQ_HZ;
    const double t_analytic = t_mem_bw + t_lat_all;

    // ---- 计算侧指标 ----
    const double flops       = double(pe.flops());               // 总浮点运算量
    const double thru_flops  = sim_s > 0 ? flops / sim_s : 0.0;   // 实际吞吐 (FLOP/s)
    const double peak_flops  = cfg.peak_compute_flops();          // 峰值算力
    const double util_pct    = peak_flops > 0 ? thru_flops / peak_flops * 100.0 : 0.0;
    const double ai          = moved > 0 ? flops / moved : 0.0;   // arithmetic intensity

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "==== NPU Perf Model  (MVP-3 / AT + PE) ====\n";
    std::cout << "workload      : M=" << t.M() << " K=" << t.K() << " N=" << t.N()
              << "  (" << cfg.data_bytes() << "B/elem)\n";
    std::cout << "config        : array_n=" << cfg.array_n()
              << " buffer_kb=" << cfg.buffer_kb()
              << " hbm_bw=" << cfg.hbm_bw_GBps() << "GB/s"
              << " hbm_lat=" << cfg.hbm_lat_cyc() << "cyc"
              << " double_buffer=" << (cfg.double_buffer() ? "on" : "off") << "\n";
    std::cout << "bytes moved   : " << moved << " B"
              << "  (Bytes_min=" << bytes_min(cfg, t) << " B)\n";
    std::cout << "dma xfers     : " << dma.num_xfers()
              << " | mem reqs=" << mem.serviced_reqs() << "\n";
    std::cout << "pe passes     : " << pe.passes()
              << " | MACs=" << double(pe.macs())
              << " | FLOP=" << flops << "\n";
    std::cout << "sim time      : " << sim_s * 1e6 << " us\n";
    std::cout << "achieved BW   : " << bw_ach_GBps << " GB/s"
              << "  (peak=" << cfg.hbm_bw_GBps() << " GB/s)\n";
    std::cout << "throughput    : " << thru_flops / 1e9 << " GFLOP/s"
              << "  (peak=" << peak_flops / 1e9 << " GFLOP/s)\n";
    std::cout << "utilization   : " << util_pct << " %\n";
    std::cout << "arith intensity: " << ai << " FLOP/B\n";
    std::cout << "serial T_mem  : " << t_analytic * 1e6 << " us"
              << "  (bw=" << t_mem_bw * 1e6 << " + lat="
              << t_lat_all * 1e6 << ")\n";

    // CSV 一行（便于 scripts/plot_results.py 汇总 roofline / 敏感性）
    std::cout << "CSV,M,K,N,array_n,buffer_kb,bytes,FLOP,sim_us,bw_GBps,GFLOPs,util_pct,AI\n";
    std::cout << "CSV," << t.M() << "," << t.K() << "," << t.N() << ","
              << cfg.array_n() << "," << cfg.buffer_kb() << ","
              << moved << "," << flops << "," << sim_s * 1e6 << ","
              << bw_ach_GBps << "," << thru_flops / 1e9 << ","
              << util_pct << "," << ai << "\n";
}
