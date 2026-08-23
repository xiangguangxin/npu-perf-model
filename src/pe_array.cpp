// PeArray 的实现：weight-stationary 脉动阵列的 cycle-approximate timing。
#include "pe_array.h"

namespace npu_perf {

// 算一趟的时间 = fill + steady + drain = 3N cycle
sc_time PeArray::pass_time() const {
    const uint64_t n = cfg_.array_n();
    return cycles(n /*fill*/ + n /*steady*/ + n /*drain*/);
}

// 登记一趟做了多少 MAC（N³），供吞吐/利用率统计
void PeArray::account_pass() {
    const uint64_t n = cfg_.array_n();
    macs_   += n * n * n;
    passes_ += 1;
}

}  // namespace npu_perf
