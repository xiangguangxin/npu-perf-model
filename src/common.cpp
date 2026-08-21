// common.h 的实现：cycle<->time 换算、NpuConfig 派生量、payload 扩展的克隆。
#include "common.h"

// ---- cycle 数 -> sc_time ----
sc_time cycles(uint64_t n) {
    return sc_time(double(n) / CLK_FREQ_HZ, SC_SEC);
}

// ---- NpuConfig 派生量 ----
double NpuConfig::peak_compute_flops() const {
    // 2×N²×f：N×N 个 PE 各每周期 1 MAC(=2 FLOP)，再乘频率得每秒运算次数
    return 2.0 * double(array_n_) * double(array_n_) * CLK_FREQ_HZ;
}

// 带宽转换函数 将“吉字节每秒”（GB/s）的带宽数据，换算为“字节每秒”（B/s）
// 1 GB/s= 10^9 B/s
double NpuConfig::hbm_bw_Bps() const { return hbm_bw_GBps_ * 1e9; } 

// 缓存大小转换函数 1 KB = 1024 字节
uint64_t NpuConfig::buffer_bytes() const { return uint64_t(buffer_kb_) * 1024ull; }

// ---- TileExtension ----
tlm_extension_base* TileExtension::clone() const { return new TileExtension(*this); }

void TileExtension::copy_from(tlm_extension_base const& e) {
    *this = static_cast<const TileExtension&>(e);
}
