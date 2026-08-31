// MVP-4 顶层入口：解析命令行 → 组装 NpuSystem → 跑仿真 → 出报告。
// 拓扑（多 DMA 经互连仲裁，收敛到内存控制器与 HBM）见 npu_system.h。
// 用法见 print_usage() / 运行 `npu_sim --help`。

#include "common.h"
#include "npu_system.h"
#include "perf_monitor.h"
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <cerrno>
#include <cstdlib>

static void print_usage(const char* prog) {
    std::cout <<
"用法: " << prog << " [选项] [M K N] [array_n] [buffer_kb]\n"
"\n"
"  MVP-4 contention-aware NPU 性能模型（多 DMA → 互连仲裁 → MC → HBM）。\n"
"  所有参数可选，未给的用默认值；M/K/N 必须三个一起给。\n"
"\n"
"位置参数:\n"
"  M           GEMM 左矩阵行数    (A: M×K)            [默认 512]\n"
"  K           GEMM 收缩维        (A 列 = B 行)       [默认 512]\n"
"  N           GEMM 右矩阵列数    (B: K×N)            [默认 512]\n"
"  array_n     脉动阵列边长 (array_n×array_n 个 PE)   [默认 16]\n"
"  buffer_kb   片上 buffer 容量 (KB)                  [默认 256]\n"
"\n"
"选项:\n"
"  -h, --help          显示本帮助并退出\n"
"  --serial            关闭 double buffering（搬和算串行，作对照）\n"
"  --dma N             并发 DMA 引擎数（1/2/4/8...）   [默认 1]\n"
"  --arbiter P         仲裁策略: fifo|rr|priority       [默认 fifo]\n"
"  --noc-latency N     互连单跳延迟 (cycle)             [默认 0]\n"
"  --queue-depth N     互连请求队列深度（背压阈值）     [默认 16]\n"
"  --interconnect-bw G 互连带宽 (GB/s)                  [默认 256]\n"
"\n"
"示例:\n"
"  " << prog << " --dma 4 --arbiter rr --noc-latency 2   # 4 DMA 竞争，RR 仲裁\n"
"  " << prog << " 512 512 512 16 256                  # 单 DMA（与 MVP-3 等价）\n"
"  " << prog << " --arbiter priority --queue-depth 4   # 优先级仲裁 + 浅队列背压\n";
}

// 把命令行字符串解析成正整数(>0)，并做严格校验；失败则打印错误 + 用法后退出。
static uint32_t parse_positive(const char* prog, const char* name, const char* s) {
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(s, &end, 10);

    if (end == s || *end != '\0') {
        std::cerr << "错误: 参数 " << name << " = \"" << s << "\" 不是合法整数\n\n";
        print_usage(prog);
        std::exit(2);
    }
    if (errno != 0 || v <= 0 || v > UINT32_MAX) {
        std::cerr << "错误: 参数 " << name << " = \"" << s
                  << "\" 必须是 1 .. " << UINT32_MAX << " 之间的正整数\n\n";
        print_usage(prog);
        std::exit(2);
    }
    return static_cast<uint32_t>(v);
}

// 解析正 double（带宽用），失败退出。
static double parse_positive_double(const char* prog, const char* name, const char* s) {
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (end == s || *end != '\0' || errno != 0 || !(v > 0.0)) {
        std::cerr << "错误: 参数 " << name << " = \"" << s << "\" 必须是正数\n\n";
        print_usage(prog);
        std::exit(2);
    }
    return v;
}

int sc_main(int argc, char* argv[]) {
    using namespace npu_perf;

    const char* prog = argv[0];

    // ---- 先扫一遍：分出选项(flag) 与 位置参数 ----
    bool double_buffer = true;
    std::vector<const char*> pos;   // 位置参数（M K N array_n buffer_kb）
    uint32_t dma_count   = 1;
    ArbiterPolicy arb    = ArbiterPolicy::FIFO;
    uint32_t noc_latency = 0;
    uint32_t queue_depth = 16;
    double   ic_bw_GBps  = 256;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "-h" || a == "--help") { print_usage(prog); return 0; }
        else if (a == "--serial")            { double_buffer = false; }
        else if (a == "--dma") {
            if (i + 1 >= argc) { std::cerr << "错误: --dma 需要一个值\n\n"; print_usage(prog); return 2; }
            dma_count = parse_positive(prog, "--dma", argv[++i]);
        }
        else if (a == "--arbiter") {
            if (i + 1 >= argc) { std::cerr << "错误: --arbiter 需要一个值\n\n"; print_usage(prog); return 2; }
            if (!parse_arbiter(argv[++i], arb)) {
                std::cerr << "错误: 未知仲裁策略 \"" << argv[i]
                          << "\"，可选 fifo|rr|priority\n\n";
                print_usage(prog); return 2;
            }
        }
        else if (a == "--noc-latency") {
            if (i + 1 >= argc) { std::cerr << "错误: --noc-latency 需要一个值\n\n"; print_usage(prog); return 2; }
            noc_latency = parse_positive(prog, "--noc-latency", argv[++i]);
        }
        else if (a == "--queue-depth") {
            if (i + 1 >= argc) { std::cerr << "错误: --queue-depth 需要一个值\n\n"; print_usage(prog); return 2; }
            queue_depth = parse_positive(prog, "--queue-depth", argv[++i]);
        }
        else if (a == "--interconnect-bw") {
            if (i + 1 >= argc) { std::cerr << "错误: --interconnect-bw 需要一个值\n\n"; print_usage(prog); return 2; }
            ic_bw_GBps = parse_positive_double(prog, "--interconnect-bw", argv[++i]);
        }
        else { pos.push_back(argv[i]); }
    }

    // ---- 参数个数校验：只接受 0 / 3 / 4 / 5 个位置参数 ----
    const int n = static_cast<int>(pos.size());
    if (n != 0 && n != 3 && n != 4 && n != 5) {
        std::cerr << "错误: 位置参数个数为 " << n
                  << "，只接受 0、3、4 或 5 个（M/K/N 必须成组给）\n\n";
        print_usage(prog);
        return 2;
    }

    NpuConfig cfg;
    cfg.set_double_buffer(double_buffer);
    cfg.set_dma_count(dma_count);
    cfg.set_arbiter_policy(arb);
    cfg.set_noc_latency(noc_latency);
    cfg.set_queue_depth(queue_depth);
    cfg.set_interconnect_bw_GBps(ic_bw_GBps);
    GemmTask task{512, 512, 512};

    if (n >= 3) {
        task.set_M(parse_positive(prog, "M", pos[0]));
        task.set_K(parse_positive(prog, "K", pos[1]));
        task.set_N(parse_positive(prog, "N", pos[2]));
    }
    if (n >= 4) cfg.set_array_n(parse_positive(prog, "array_n",   pos[3]));
    if (n >= 5) cfg.set_buffer_kb(parse_positive(prog, "buffer_kb", pos[4]));

    // buffer 至少要装得下一个 tile，否则数据流无意义
    const uint64_t tile_bytes = uint64_t(cfg.array_n()) * cfg.array_n() * cfg.data_bytes();
    if (cfg.buffer_bytes() < tile_bytes) {
        std::cerr << "错误: buffer_kb=" << cfg.buffer_kb() << " (="
                  << cfg.buffer_bytes() << "B) 装不下一个 tile ("
                  << tile_bytes << "B)，请增大 buffer 或减小 array_n\n";
        return 2;
    }

    // ---- 组装并运行 ----
    NpuSystem sys("npu", cfg, task);
    sys.run();

    PerfMonitor::report(cfg, task, sys.interconnect(), sys.mc(), sys.hbm(),
                        sys.pe(), sys.driver());
    return 0;
}
