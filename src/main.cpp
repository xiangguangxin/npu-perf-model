// MVP-3 顶层组装：Memory + Buffer + 单 DMA + PE + Driver，AT 直连跑通搬+算。
// 拓扑（MVP-2 简化，先跳过 Interconnect）：
//     DmaEngine.isock ──nb_transport (4 phases)──> Memory.tsock
// 用法见 print_usage() / 运行 `npu_sim --help`。

#include "common.h"
#include "memory.h"
#include "onchip_buffer.h"
#include "dma_engine.h"
#include "pe_array.h"
#include "workload_driver.h"
#include "perf_monitor.h"
#include <iostream>
#include <string>
#include <vector>
#include <cstdint>
#include <cerrno>
#include <cstdlib>

static void print_usage(const char* prog) {
    std::cout <<
"用法: " << prog << " [M K N] [array_n] [buffer_kb]\n"
"\n"
"  MVP-3 AT 数据流性能模型。所有参数可选，未给的用默认值；\n"
"  M/K/N 必须三个一起给（要么都默认，要么都覆盖）。\n"
"\n"
"位置参数:\n"
"  M           GEMM 左矩阵行数    (A: M×K)            [默认 512]\n"
"  K           GEMM 收缩维        (A 列 = B 行)       [默认 512]\n"
"  N           GEMM 右矩阵列数    (B: K×N)            [默认 512]\n"
"  array_n     脉动阵列边长 (array_n×array_n 个 PE)   [默认 16]\n"
"  buffer_kb   片上 buffer 容量 (KB)                  [默认 256]\n"
"\n"
"  注意: GEMM 的 N 与阵列边长 array_n 是两个不同的量，勿混淆。\n"
"\n"
"选项:\n"
"  -h, --help  显示本帮助并退出\n"
"  --serial    关闭 double buffering（搬和算串行，作对照）；默认开启\n"
"\n"
"示例:\n"
"  " << prog << "                       # 全默认: 512^3, array_n=16, buffer=256KB, 双缓冲\n"
"  " << prog << " 128 256 64            # 只改 GEMM 形状\n"
"  " << prog << " 512 512 512 32        # 阵列换成 32×32\n"
"  " << prog << " --serial 512 512 512  # 关掉双缓冲，看串行耗时\n";
}

// 把命令行字符串解析成正整数(>0)，并做严格校验；失败则打印错误 + 用法后退出。
static uint32_t parse_positive(const char* prog, const char* name, const char* s) {
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(s, &end, 10);

    if (end == s || *end != '\0') {   // 有非数字字符 / 空串
        std::cerr << "错误: 参数 " << name << " = \"" << s
                  << "\" 不是合法整数\n\n";
        print_usage(prog);
        std::exit(2);
    }
    if (errno != 0 || v <= 0 || v > UINT32_MAX) {  // 溢出 / 非正 / 越界
        std::cerr << "错误: 参数 " << name << " = \"" << s
                  << "\" 必须是 1 .. " << UINT32_MAX << " 之间的正整数\n\n";
        print_usage(prog);
        std::exit(2);
    }
    return static_cast<uint32_t>(v);
}

int sc_main(int argc, char* argv[]) {
    const char* prog = argv[0];

    // ---- 先扫一遍：分出选项(flag) 与 位置参数 ----
    bool double_buffer = true;
    std::vector<const char*> pos;   // 位置参数（M K N array_n buffer_kb）
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { print_usage(prog); return 0; }
        else if (a == "--serial")       { double_buffer = false; }
        else                            { pos.push_back(argv[i]); }
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
    GemmTask  task{512, 512, 512};

    // ---- 命令行覆盖（渐进式，带校验）----
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

    // ---- 组装拓扑 ----
    Memory         mem("mem", cfg);
    OnchipBuffer   buf("buf", cfg);
    DmaEngine      dma("dma", cfg);
    PeArray        pe("pe", cfg);
    WorkloadDriver drv("drv", cfg, task, &dma, &buf, &pe);

    // MVP-2：DMA 直连 Memory（Interconnect 是加分模块，后续再插）
    dma.isock.bind(mem.tsock);

    sc_start();

    PerfMonitor::report(cfg, task, mem, dma, pe, drv);
    return 0;
}
