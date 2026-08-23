#pragma once
// 全局类型、硬件配置、cycle<->time 换算、payload 扩展。
// MVP-3：DMA↔Memory 使用 TLM AT 路径；其他组件保持 timing abstraction。

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#include <cstdint>
#include <cmath>

using namespace sc_core;
using namespace tlm;

namespace npu_perf {

// ---- 时钟与换算 ----
constexpr double CLK_FREQ_HZ = 1.0e9;          // 1 GHz 单位每秒多少个周期 Hz = 1秒(每秒多少个周期)

// cycle 数 -> sc_time
sc_time cycles(uint64_t n);

// ---- 硬件配置（可从命令行/配置文件注入）----
// 数据封装成 private 字段：对外只读靠 getter，能被命令行覆盖的量才给 setter。
class NpuConfig {
public:
    // ---- 派生量 ----
    // 峰值算力 (FLOP/s)：N² 个 MAC，每 MAC 2 FLOP
    // 锋值算力 = PE数量 * 每PE每周期MAC次数 * MAC 的FLOP数 *频率 = 2×N²×f
    // (N*N = PE数量；一个MAC(乘加)是 2 FLOP；周期与频率互为倒数，乘频率=每秒运算次数)
    double peak_compute_flops() const;
    // HBM 带宽换算成 Bytes/s
    double hbm_bw_Bps() const;
    // buffer 总容量 (Bytes)
    uint64_t buffer_bytes() const;

    // ---- 只读访问器 ----
    uint32_t array_n()         const { return array_n_; }           // 脉动阵列边长 N (N×N PE)
    uint32_t buffer_kb()       const { return buffer_kb_; }         // 片上 buffer 容量 (KB)
    double   buf_bw_Bpc()      const { return buf_bw_Bpc_; }        // buffer 带宽 (Bytes/cycle)
    double   hbm_bw_GBps()     const { return hbm_bw_GBps_; }       // HBM 带宽 (GB/s)
    uint32_t hbm_lat_cyc()     const { return hbm_lat_cyc_; }       // HBM 访问延迟 (cycle)
    uint32_t dma_outstanding() const { return dma_outstanding_; }   // DMA 最大未完成事务
    bool     double_buffer()   const { return double_buffer_; }     // 是否开启预取重叠
    uint32_t data_bytes()      const { return data_bytes_; }        // 每元素字节数 (int8 = 1)

    // ---- 命令行会覆盖的量才暴露 setter ----
    void set_array_n(uint32_t v)      { array_n_ = v; }
    void set_buffer_kb(uint32_t v)    { buffer_kb_ = v; }
    void set_double_buffer(bool v)    { double_buffer_ = v; }

private:
    uint32_t array_n_        = 16;      // 脉动阵列边长 N (N×N PE)
    uint32_t buffer_kb_      = 256;     // 片上 buffer 容量 (KB)
    double   buf_bw_Bpc_     = 64;      // buffer 带宽 (Bytes per cycle)， 每个 clock cycle 可以传输 64 Bytes 数据， Buffer通常和芯片内部时钟绑定
    double   hbm_bw_GBps_    = 256;     // HBM (High Bandwidth Memory高带宽内存)带宽 (GB/s)， 每秒传输 256 GB数据， 系统级内存
    uint32_t hbm_lat_cyc_    = 100;     // HBM 访问延迟 (cycle)
    uint32_t dma_outstanding_ = 4;      // DMA 最大未完成 AT 事务
    bool     double_buffer_  = true;    // 是否开启预取重叠（MVP-2 才用到）
    uint32_t data_bytes_     = 1;       // 每元素字节数 (int8 = 1)
};

// ---- GEMM workload 描述 ---- General Matrix Multiply
// GemmTask 用来定义矩阵的总计算量（Compute Demand）和总访存量（Memory Traffic）
// 输入 workload 的形状：M/K/N 三个维度，构造时给定，命令行可逐个覆盖。
// C[M × N] = A[M × K] × B[K × N]
class GemmTask {
public:
    GemmTask(uint32_t M = 0, uint32_t K = 0, uint32_t N = 0)
        : M_(M), K_(K), N_(N) {}

    uint32_t M() const { return M_; }
    uint32_t K() const { return K_; }
    uint32_t N() const { return N_; }

    void set_M(uint32_t v) { M_ = v; }
    void set_K(uint32_t v) { K_ = v; }
    void set_N(uint32_t v) { N_ = v; }

private:
    uint32_t M_, K_, N_;
};

// A 的列数必须等于 B 的行数
// ---- 具体例子（M=2, K=3, N=2）----
//      A (2×3)       B (3×2)        C (2×2)
//    [ 1  2  3 ]    [ 7  8 ]      [ 31  19 ]
//    [ 4  5  6 ]  × [ 9  1 ]  =   [ 85  55 ]
//                   [ 2  3 ]

// ---- 用扩展把"这是哪种数据/哪个 tile"挂在 payload 上 ----
class TileExtension : public tlm_extension<TileExtension> {
public:
    enum Kind { WEIGHT, ACTIVATION, OUTPUT };

    Kind     kind()    const { return kind_; }
    uint32_t tile_id() const { return tile_id_; }
    void set_kind(Kind k)        { kind_ = k; }
    void set_tile_id(uint32_t t) { tile_id_ = t; }

    tlm_extension_base* clone() const override;
    void copy_from(tlm_extension_base const& e) override;

private:
    Kind     kind_    = WEIGHT;
    uint32_t tile_id_ = 0;
};

}  // namespace npu_perf
