#pragma once
// ============================================================================
// Arbiter —— MVP-4 的仲裁策略（Development order Step 2 / §6）。
//
// 关键设计点：Arbiter 不是 sc_module，而是一个纯 C++ 类——它没有独立时序，
// 只是从"候选请求队列"里挑一个出来。时序归 Interconnect 所有（见 §9 Timing
// Ownership：Arbiter 只做 selection）。因此：
//   - 不继承 sc_module、不挂 socket、不 wait；
//   - 只实现 select(queue) 这一个纯函数式职责，便于换策略做对比实验。
//
// 三种策略（§6）：
//   FIFO        —— 先到先服务；
//   Round Robin —— 在多 source 间轮流，保证公平、避免饿死；
//   Priority    —— 按 source id 定优先级（id 越小优先级越高），做 QoS。
// ============================================================================

#include "common.h"
#include "request.h"
#include <deque>
#include <memory>

namespace npu_perf {

// 抽象基类：从候选队列挑一个请求（只返回指针，移除由调用方负责）。
class Arbiter {
public:
    virtual ~Arbiter() = default;
    virtual MemoryRequest* select(std::deque<MemoryRequest*>& queue) = 0;
    virtual const char* name() const = 0;
};

// FIFO：先到先服务，取队首。
class FIFOArbiter : public Arbiter {
public:
    MemoryRequest* select(std::deque<MemoryRequest*>& queue) override {
        return queue.front();
    }
    const char* name() const override { return "fifo"; }
};

// Round Robin：以 source 为粒度轮流。记住上次授予的 source，下一轮从其后开始
// 找第一个"有请求"的源，避免某个 DMA 长期霸占互连。
class RoundRobinArbiter : public Arbiter {
public:
    explicit RoundRobinArbiter(uint32_t num_sources) : num_sources_(num_sources) {}

    MemoryRequest* select(std::deque<MemoryRequest*>& queue) override {
        // 从 last_ 的下一个 source 开始，循环一圈找第一个有请求的源。
        for (uint32_t step = 1; step <= num_sources_; ++step) {
            uint32_t src = (last_ + step) % num_sources_;
            for (auto* r : queue) {
                if (r->source() == src) { last_ = src; return r; }
            }
        }
        // 兜底（理论上到不了这里）：取队首并记住其源。
        last_ = queue.front()->source();
        return queue.front();
    }
    const char* name() const override { return "round-robin"; }

private:
    uint32_t num_sources_;
    uint32_t last_ = 0;   // 上次授予的 source
};

// Priority：source id 越小优先级越高；同优先级按入队先后（稳定）。
class PriorityArbiter : public Arbiter {
public:
    MemoryRequest* select(std::deque<MemoryRequest*>& queue) override {
        MemoryRequest* best = queue.front();
        for (auto* r : queue) {
            if (r->source() < best->source()) best = r;
        }
        return best;
    }
    const char* name() const override { return "priority"; }
};

// 工厂：按 NpuConfig::ArbiterPolicy 构造对应策略。
std::unique_ptr<Arbiter> make_arbiter(ArbiterPolicy p, uint32_t num_sources);

}  // namespace npu_perf
