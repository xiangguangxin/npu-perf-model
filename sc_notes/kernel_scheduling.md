# 仿真内核与调度(最容易被深挖)

## 内核调度总览:5 阶段 + 两层循环

按 IEEE 1666(LRM),`sc_start()` 启动的调度器分 **5 个阶段**;下面几节讲的 delta cycle 只是其中的内层循环。先建立全局:

| # | 阶段 | 做什么 |
|---|---|---|
| **1** | **Initialization(初始化)** | 每个 process(除非 `dont_initialize()`)先跑一次,放进 runnable 集合;**只发生一次** |
| **2** | **Evaluation(评估)** | 从 runnable 集合挑进程执行(METHOD 到 return / THREAD 到下个 `wait`)。写 signal、notify 只"登记"不生效。反复执行直到 runnable 为空 |
| **3** | **Update(更新)** | 统一处理 evaluate 里挂起的 `request_update()`——signal 新值这时才真正生效 |
| **4** | **Delta notification(delta 通知)** | 处理值变化产生的通知 + `notify(SC_ZERO_TIME)`,把敏感进程标记为 runnable |
| **5** | **Timed notification(时间推进)** | 前面都空了,才去 timed 队列,把 `sc_time_stamp()` 推进到最近事件点,唤醒对应进程 |

这 5 步是**两层嵌套循环**:

```
初始化(1) ── 只一次
   │
   ▼
┌─────────── delta cycle(内层循环)───────────┐
│  Evaluate(2) → Update(3) → Delta-notify(4) │
│         ▲                        │           │
│         └──── 有新 runnable? ────┘           │  仿真时间不变(delta count+1)
└──────────── runnable 空了才出来 ─────────────┘
   │
   ▼
Timed advance(5) ── 时间跳到下个事件点
   │
   └──────── 回到 delta cycle,循环,直到无 pending 事件 ───────────
```

- **内层 = delta cycle**(2→3→4):同一时刻反复迭代,`sc_time_stamp()` **不变**;详见下一节。
- **外层 = 时间推进**(5):内层彻底空了,时间才前进一次,然后又进入新一轮 delta cycle。

> **对应本项目**:LT 模型里 `wait(delay)` 用的是 **timed** notification,主要在**第 5 步**反复跳(那 322us 就是这么累加的);MVP-1 几乎没有 signal/零延时事件,delta cycle 那层一带而过。等 MVP-3 上 AT + PEQ,零延时/delta 事件才会热闹起来。

## Delta Cycle 完整机制

Evaluate → Update → Delta notification 三阶段循环,在同一个仿真时刻(`sc_time_stamp()` 不变)反复迭代:

1. **Evaluate**:内核跑完所有当前 runnable 的 process(`SC_METHOD` 到 return,`SC_THREAD` 到下一个 `wait()`)。期间对 signal 的写入、对事件的 delta/timed notify 都只是"登记",不会立即生效。
2. **Update**:统一处理 evaluate 阶段里挂起的更新请求(`request_update()`)——signal 的新值这时才真正写入、变得可读。
3. **Delta notification**:处理因为值变化或显式 `notify(SC_ZERO_TIME)` 产生的通知,把敏感进程标记为下一个 delta 的 runnable。
4. 如果第 3 步产生了新的 runnable process,回到第 1 步(delta count + 1,仿真时间不变);否则内核才去看 timed event 队列、把时间推进到下一个事件点。

**追问**:"为什么 `sc_signal` 写入后,当前 delta 读不到新值?"——因为 `write()` 只是调用了 `request_update()`,把新值挂起来,真正赋值发生在 **update 阶段**,而 update 阶段要等本 delta 内所有 evaluate 都跑完才会执行。这保证了同一 delta 内,不管进程执行顺序如何,大家读到的都是本 delta 开始时的旧值。

## 三种 notify 的区别

```cpp
e.notify();               // ① immediate notification
e.notify(SC_ZERO_TIME);   // ② delta notification
e.notify(sc_time(10, SC_NS)); // ③ timed notification
```

| | 触发时机 | 是否排队 | 确定性 |
|---|---|---|---|
| ① `notify()` | **立即**,就在当前 evaluate 阶段内直接触发 | 不排队,直接唤醒此刻正在等待的进程 | **不确定**,依赖进程执行顺序 |
| ② `notify(SC_ZERO_TIME)` | 下一个 delta cycle | 排进 delta 通知队列 | 确定,不管调用时机,保证下一个 delta 生效 |
| ③ `notify(t)`,t > 0 | `sc_time_stamp() + t` | 排进 timed event 队列 | 确定 |

- **①为什么危险**:immediate notification 不走排队机制,只对"此刻正挂起等待该事件"的进程立即生效。如果某个进程这一 delta 里还没执行到 `wait(e)`(还没登记等待),或者它已经在本 delta 跑过、不会再被重新调度,它就会**完全错过**这次通知——不像 delta/timed notify 那样会被排队、保证下一轮一定送达。这使得行为依赖于"谁先执行到 wait、谁后调用 notify"这种进程调度顺序,是不确定的,LRM 明确不建议依赖它。
- ②、③ 都是排队式的,不管 `notify()` 是在 evaluate 阶段的哪个时间点被调用,效果都一样确定——这也是为什么绝大多数模型用 `notify(SC_ZERO_TIME)` 而不是无参 `notify()`。

## 同一事件多次 notify 的覆盖规则

一个事件在任意时刻**最多只有一个 pending 的(delta/timed)notification** 在排队。如果对同一个事件再次调用 `notify()`,规则是:**两次里预定触发时间更早的那次生效,更晚的那次被丢弃**——和这两次调用在代码里的先后顺序无关。

```cpp
e.notify(sc_time(10, SC_NS));  // 先注册:预定 10ns 后触发
e.notify(sc_time(5, SC_NS));   // 后注册,但时间更早 → 覆盖前一个,变成 5ns 后触发

// 反过来:
e.notify(sc_time(5, SC_NS));   // 先注册:5ns
e.notify(sc_time(10, SC_NS));  // 后注册,但时间更晚 → 无效果,pending 的还是 5ns
```

(该规则针对 delta/timed notification;immediate notification 本身不排队,不适用此规则。)

## 进程执行顺序的不确定性

同一 delta 内,多个 runnable process 之间的**执行顺序标准未定义**(不同实现可能有不同的内部顺序,比如按注册顺序,但这不是规范保证的行为)。正确的模型**不能依赖**这个顺序——不能假设"A 一定比 B 先跑"。

这也直接解释了 `sc_signal` 为什么要做"双缓冲"式的 update 语义:如果 signal 写入立即生效,那么同一 delta 内谁先执行、谁后执行就会导致不同的读取结果,模型行为就变成了调度器实现细节的产物,而不是设计语义本身决定的。把"写入生效"推迟到统一的 update 阶段,就从根本上消除了这种因为执行顺序不确定而产生的竞态。

## 仿真阶段回调:elaboration vs simulation

SystemC 模块/channel 有几个可以重载的生命周期回调,对应 elaboration 与 simulation 的分界:

| 回调 | 调用时机 | 典型用途 |
|---|---|---|
| `before_end_of_elaboration()` | 构造之后,elaboration **还没**结束前(可能被多次调用,因为此时结构还可能被继续修改) | 需要依赖"其他模块可能还没构造完"这个前提去做的补充绑定/构造 |
| `end_of_elaboration()` | 整个 elaboration(结构、binding)**已经固定**,simulation 开始之前,只调用一次 | 做最终的合法性检查——比如 TLM 里检查所有 socket 是否都已绑定、做 DMI 区域的初始发现/协商 |
| `start_of_simulation()` | 第一次 `sc_start()` 真正开始跑 process 之前,只调用一次 | 需要"仿真即将真正开始"这个时间点的初始化,比如打开 trace 文件、把统计计数器清零到仿真时间 0 |
| `end_of_simulation()` | `sc_stop()` 被调用或仿真自然结束时 | 收尾工作——关闭文件、打印统计汇总、flush 日志 |

**追问关联**:TLM 里"绑定检查"和"DMI 初始发现"常放在 `end_of_elaboration()` 里做,是因为这是**结构第一次被保证不再变化**的时间点——在这之前做检查可能因为后续还有 socket 会被绑定而误报。
