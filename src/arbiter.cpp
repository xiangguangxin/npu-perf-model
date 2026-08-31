// Arbiter 工厂的实现。
#include "arbiter.h"

namespace npu_perf {

std::unique_ptr<Arbiter> make_arbiter(ArbiterPolicy p, uint32_t num_sources) {
    switch (p) {
        case ArbiterPolicy::ROUND_ROBIN:
            return std::make_unique<RoundRobinArbiter>(num_sources);
        case ArbiterPolicy::PRIORITY:
            return std::make_unique<PriorityArbiter>();
        case ArbiterPolicy::FIFO:
        default:
            return std::make_unique<FIFOArbiter>();
    }
}

}  // namespace npu_perf
