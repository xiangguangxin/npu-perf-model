// OnchipBuffer 的实现：片上 SRAM 的容量约束(can_hold) + 带宽约束(access_time)。
#include "onchip_buffer.h"

namespace npu_perf {

// ---- 容量约束 ----
// 再塞 bytes 字节会不会超过 buffer 总容量？(uint64_t 相加防 32 位溢出)
bool OnchipBuffer::can_hold(uint32_t bytes) const {
    return uint64_t(used_bytes_) + bytes <= cfg_.buffer_bytes();
}

// 占位：登记又用掉了 bytes，并顺手更新历史占用峰值(供敏感性分析)。
void OnchipBuffer::allocate(uint32_t bytes) {
    used_bytes_ += bytes;
    if (used_bytes_ > peak_bytes_) peak_bytes_ = used_bytes_;
}

// 腾空：释放 bytes；防下溢，释放量 >= 当前占用时直接清零。
void OnchipBuffer::release(uint32_t bytes) {
    used_bytes_ = (bytes >= used_bytes_) ? 0 : used_bytes_ - bytes;
}

// ---- 带宽约束 ----
// 读/写一块 bytes 数据占用的时间 = ceil(bytes / buffer 带宽) 个 cycle。
sc_time OnchipBuffer::access_time(uint32_t bytes) const {
    return cycles(uint64_t(std::ceil(double(bytes) / cfg_.buf_bw_Bpc())));
}

}  // namespace npu_perf
