// PerfMonitor 的实现：仿真结束后汇总访存/计算/竞争侧指标，打印报告 + CSV 一行。
#include "perf_monitor.h"
#include <iostream>
#include <iomanip>
#include <algorithm>

namespace npu_perf {

// 理想最小搬运字节（无复用惩罚）
double PerfMonitor::bytes_min(const NpuConfig& c, const GemmTask& t) {
    return (double(t.M()) * t.K() + double(t.K()) * t.N() + double(t.M()) * t.N())
         * c.data_bytes();
}

// 仲裁策略名（只用于打印）。
static const char* arbiter_name(ArbiterPolicy p) {
    switch (p) {
        case ArbiterPolicy::ROUND_ROBIN: return "round-robin";
        case ArbiterPolicy::PRIORITY:    return "priority";
        case ArbiterPolicy::FIFO:
        default:                         return "fifo";
    }
}

void PerfMonitor::report(const NpuConfig& cfg, const GemmTask& t,
                         const Interconnect& ic, const MemoryController& mc,
                         const Hbm& hbm, const PeArray& pe,
                         const WorkloadDriver& drv) {
    const double sim_s       = drv.run_time().to_seconds();
    const double moved       = double(mc.serviced_bytes());   // 全部 DMA 流量都经 MC
    const double bw_ach_GBps = sim_s > 0 ? moved / sim_s / 1e9 : 0.0;
    const double bw_eff_GBps = std::min(cfg.interconnect_bw_GBps(), cfg.hbm_bw_GBps());

    // 解析下界：纯带宽时间 + 每笔固定延迟（AT outstanding 可隐藏一部分）。
    const double t_mem_bw    = moved / cfg.hbm_bw_Bps();
    const double t_lat_all   = double(mc.serviced_reqs()) * cfg.hbm_lat_cyc() / CLK_FREQ_HZ;
    const double t_analytic  = t_mem_bw + t_lat_all;

    // ---- 计算侧指标 ----
    const double flops       = double(pe.flops());
    const double thru_flops  = sim_s > 0 ? flops / sim_s : 0.0;
    const double peak_flops  = cfg.peak_compute_flops();
    const double util_pct    = peak_flops > 0 ? thru_flops / peak_flops * 100.0 : 0.0;
    const double ai          = moved > 0 ? flops / moved : 0.0;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "==== NPU Perf Model  (MVP-4 / Interconnect + Arbitration) ====\n";
    std::cout << "workload      : M=" << t.M() << " K=" << t.K() << " N=" << t.N()
              << "  (" << cfg.data_bytes() << "B/elem)\n";
    std::cout << "config        : array_n=" << cfg.array_n()
              << " buffer_kb=" << cfg.buffer_kb()
              << " hbm_bw=" << cfg.hbm_bw_GBps() << "GB/s"
              << " hbm_lat=" << cfg.hbm_lat_cyc() << "cyc"
              << " double_buffer=" << (cfg.double_buffer() ? "on" : "off") << "\n";
    std::cout << "fabric        : dma=" << cfg.dma_count()
              << " arbiter=" << arbiter_name(cfg.arbiter_policy())
              << " noc_latency=" << cfg.noc_latency() << "cyc"
              << " queue_depth=" << cfg.queue_depth()
              << " interconnect_bw=" << cfg.interconnect_bw_GBps() << "GB/s\n";
    std::cout << "bytes moved   : " << moved << " B"
              << "  (Bytes_min=" << bytes_min(cfg, t) << " B)\n";
    std::cout << "requests      : mem reqs=" << mc.serviced_reqs()
              << " | interconnect saw=" << ic.requests() << "\n";
    std::cout << "pe passes     : " << pe.passes()
              << " | MACs=" << double(pe.macs())
              << " | FLOP=" << flops << "\n";
    std::cout << "sim time      : " << sim_s * 1e6 << " us\n";
    std::cout << "achieved BW   : " << bw_ach_GBps << " GB/s"
              << "  (peak=" << cfg.hbm_bw_GBps()
              << ", effective=" << bw_eff_GBps << " GB/s)\n";
    std::cout << "throughput    : " << thru_flops / 1e9 << " GFLOP/s"
              << "  (peak=" << peak_flops / 1e9 << " GFLOP/s)\n";
    std::cout << "utilization   : " << util_pct << " %\n";
    std::cout << "arith intensity: " << ai << " FLOP/B\n";
    std::cout << "contention    : avg_queue=" << ic.avg_queue_delay_ns() << "ns"
              << " | queue_full=" << ic.queue_full_events()
              << " | stall=" << ic.total_stall_ns() << "ns\n";
    // 公平性：各源被授予的请求次数（RR 应接近均衡，Priority 应偏向低 id）。
    std::cout << "grants/source : ";
    for (uint32_t i = 0; i < cfg.dma_count(); ++i)
        std::cout << (i ? " " : "") << "dma" << i << "=" << ic.granted(i);
    std::cout << "\n";
    std::cout << "serial T_mem  : " << t_analytic * 1e6 << " us"
              << "  (bw=" << t_mem_bw * 1e6 << " + lat="
              << t_lat_all * 1e6 << ")\n";

    // CSV 一行（便于 scripts/plot_results.py 汇总 roofline / 敏感性）
    std::cout << "CSV,M,K,N,array_n,buffer_kb,dma,arbiter,noc_latency,queue_depth,"
                 "bytes,FLOP,sim_us,bw_GBps,eff_bw_GBps,GFLOPs,util_pct,AI,"
                 "avg_queue_ns,queue_full,stall_ns\n";
    std::cout << "CSV," << t.M() << "," << t.K() << "," << t.N() << ","
              << cfg.array_n() << "," << cfg.buffer_kb() << ","
              << cfg.dma_count() << "," << arbiter_name(cfg.arbiter_policy()) << ","
              << cfg.noc_latency() << "," << cfg.queue_depth() << ","
              << moved << "," << flops << "," << sim_s * 1e6 << ","
              << bw_ach_GBps << "," << bw_eff_GBps << ","
              << thru_flops / 1e9 << "," << util_pct << "," << ai << ","
              << ic.avg_queue_delay_ns() << "," << ic.queue_full_events() << ","
              << ic.total_stall_ns() << "\n";
}

}  // namespace npu_perf
