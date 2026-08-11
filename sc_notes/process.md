# SystemC Process:SC_METHOD / SC_THREAD / SC_CTHREAD

## 三种进程类型对比

| | SC_METHOD | SC_THREAD | SC_CTHREAD |
|---|---|---|---|
| 能否 `wait()` | **不能**(调用即运行时错误) | 能,任意位置 | 能,但只能是 `wait()`(等下一个时钟沿)或 `wait(n)`(等 n 个时钟沿) |
| 执行模型 | 普通函数调用,一次跑到 `return` | 协程(独立栈),挂起点恢复执行 | 协程,固定挂起在时钟边沿 |
| 开销 | 低(没有上下文切换/栈保存) | 较高(需要保存/恢复栈) | 同 SC_THREAD |
| 局部变量能否跨越等待保留 | 不能(每次都是全新调用,状态必须放成员变量里) | 能(这是它最大的优势) | 能 |
| 敏感性 | 纯静态(或用 `next_trigger()` 做"下一次"级别的动态调整) | 静态 + 动态都能用 | 固定绑定单一时钟沿 |
| 典型用途 | 组合逻辑、被 PEQ 回调触发的 timing 计算(状态用成员变量维护) | 时序化/协议化建模:发起一次 transaction、AT 4-phase 状态机、testbench 激励 | 老式行为综合(HLS)流程,现代 TLM/性能建模基本不用 |

```cpp
SC_CTOR(Foo) {
    SC_METHOD(do_calc);
    sensitive << a << b;          // 静态敏感性,列表中任意事件触发就整段重跑

    SC_THREAD(run);               // 协程式,可以在函数体内部 wait()
}

void Foo::do_calc() {
    // 每次被触发都是从头执行,局部变量不会保留
}

void Foo::run() {
    while (true) {                // 几乎总是写成无限循环
        wait(req_event);          // 动态敏感:显式等某个事件
        // ...
        wait(10, SC_NS);          // 动态敏感:显式等一段时间
    }
}
```

**性能取舍**:`SC_METHOD` 没有协程上下文切换,是最快的 process 类型,适合性能敏感的 timing 计算;`SC_THREAD` 适合天然要跨越多个时间点、还要保留上下文的顺序逻辑(比如 DMA 描述符处理、AT 4-phase 状态机)。实践中很多高性能 TLM 平台(包括 `tlm_utils` 自带的 Payload Event Queue)故意用 `SC_METHOD + PEQ` 的组合去实现本该是"thread"风格的顺序逻辑,就是为了拿 METHOD 的性能又不失去时间点驱动的语义。

**易踩的坑**:`SC_THREAD` 的函数体一旦 `return`,这个 process 就被内核标记为**永久终止**,哪怕它的静态敏感性事件之后还会触发,也不会再被唤醒、不会重新从头跑——这跟 `SC_METHOD` 完全不同(`SC_METHOD` 每次触发都是"重新调用一次函数",没有"进程死掉"的概念)。所以 `SC_THREAD` 几乎都要写成 `while(true) { ...; wait(...); }`。

## 和硬件逻辑的类比(RTL 视角)

上面的对比是从 TLM/性能建模角度写的;如果换成 RTL 综合的语言,进程类型和门级逻辑有一层直觉对应:

| 硬件逻辑 | 对应进程 | 为什么像 |
|---|---|---|
| **组合逻辑** | `SC_METHOD` | 输入一变就整段重算、**无跨调用状态**、输出 = 当前输入的纯函数 |
| **时序逻辑** | `SC_CTHREAD` | **时钟沿驱动**、状态跨周期保留(就是寄存器/触发器模型) |
| (两者都不纯) | `SC_THREAD` | 通用协程,事件驱动、不绑时钟——是"超集",不对应某一种 |

- **SC_METHOD ≈ 组合逻辑**:敏感于输入电平(`sensitive << a << b`),任一输入变就重跑;每次从头执行、局部变量不保留 → **没有记忆**,正是组合逻辑"输出只由当前输入决定"的特征。
- **SC_CTHREAD ≈ 时序逻辑**:`wait()` 只能等时钟沿 → 时钟驱动;协程栈跨周期保留状态 → **有记忆**,就是触发器攒状态。
- **SC_THREAD "两者都不纯"**:它两边特征各沾一点、又都不满足——**有状态**(协程栈保留局部变量,像时序),**但不绑时钟**(`wait` 可等任意事件/时长,不像时序);更不像无记忆的组合逻辑。所以它不落在"敏感电平+无状态"(组合),也不落在"时钟沿+有状态"(时序)这两个干净的格子里。它其实是**超集**:让它 `while(true){ wait(clk.pos()); ... }` 就能退化成类时序行为,但它更常用来描述 RTL 根本表达不了的东西——跨越多个时间点的事务、AT 4-phase 握手、testbench 激励序列。组合/时序只是它能力的两个特例,它工作在比门级更高的抽象层。

**但类比绑的是"敏感性 + 有无状态",不是进程种类本身。** 最典型的反例:RTL 风格里**时序逻辑其实常用 `SC_METHOD` 写**——

```cpp
// 组合逻辑:敏感于输入电平
SC_METHOD(comb);  sensitive << a << b;      // a/b 变就重算 → 组合

// 时序逻辑:敏感于时钟沿 + 状态放成员变量
SC_METHOD(seq);   sensitive << clk.pos();   // 只在上升沿触发
void seq() { q = d; }   // q 是成员变量(跨触发保留)→ 就是一个 D 触发器
```

所以更准确的归纳:
- **组合逻辑** = 敏感输入电平 + 无状态 → 天然是 `SC_METHOD`
- **时序逻辑** = 时钟沿触发 + 有状态 → `SC_CTHREAD` 最直接,但 `SC_METHOD + sensitive(clk.pos()) + 成员变量` 才是现代 RTL 综合的主流写法(这也是 `SC_CTHREAD` 现代基本不用的原因:METHOD 敏感时钟沿更灵活、开销更低)。

> **注意本项目用不到这条类比**:npu-perf-model 是 cycle-approximate 的 **TLM 性能模型,不是 RTL**——只建 timing(一次搬运花几个周期),不建门电路/触发器。这里的 `SC_THREAD`(driver)和未来的 `SC_METHOD + PEQ`(AT 回调)都工作在事务级,不是在描述组合/时序门级行为。组合/时序是理解 SC_METHOD 无状态、SC_CTHREAD 时钟驱动的好抓手,但不是本项目的建模主线。

## 静态敏感性 vs 动态敏感性

**静态敏感性(Static Sensitivity)**:在构造函数里用 `sensitive << ...` 声明,仿真期间不变。

- 对 `SC_METHOD`:基本是唯一的触发方式——列表里任意事件发生,整个函数从头重跑一遍。
- 对 `SC_THREAD`:真正含义是"当内部调用不带参数的 `wait();` 时,默认要等待的事件集合"——它是动态无参 `wait()` 的默认条件,而不是独立的另一套触发机制。

**动态敏感性(Dynamic Sensitivity)**:只能在 `SC_THREAD`/`SC_CTHREAD` 内部用,通过显式 `wait(...)` 指定,只对这一次 `wait` 生效:

```cpp
wait(req_event);          // 等一个事件
wait(10, SC_NS);          // 等一段时间
wait(evt_a | evt_b);      // 或条件(sc_event_or_list)
wait(evt_a & evt_b);      // 与条件(sc_event_and_list)
```

`SC_METHOD` 不能调用 `wait()`,但可以用 `next_trigger()` 达到类似效果:不阻塞、立刻返回(本次调用照常结束),但会临时覆盖**下一次**的触发条件;状态还是得放在成员变量里,不像 thread 能在函数中间悬停并保留局部变量。

## Delta Cycle:为什么需要 evaluate → update → delta notification

设想两个 process:A 读 signal `s`,B 写 `s`。如果 B 一 `write()` 就立刻让新值对 A 可见,那 A 读到的值就取决于 A、B 谁先被内核调度——SystemC 并不保证同一 delta 内多个 runnable process 的执行顺序,这样就是竞争条件(race condition)。

Delta cycle 把"计算新值"和"新值生效"这两件事拆开:

1. **Evaluate(评估)**:内核挑出所有当前 runnable 的 process 执行(`SC_METHOD` 跑到返回,`SC_THREAD` 跑到下一个 `wait()`)。期间调用 `signal.write(v)` 不会立即生效,只是注册一个"待更新"请求;`event.notify(SC_ZERO_TIME)` 也只是把通知排进 delta 队列。本阶段反复执行直到没有更多 runnable process。
2. **Update(更新)**:统一处理 evaluate 阶段挂起的更新请求——signal 新值这时才真正生效、变得可见,且是批量同时生效,不依赖 evaluate 阶段的执行顺序。
3. **Delta notification(delta 通知)**:处理 update 阶段中"值确实变了"而产生的即时通知,以及用户显式的零延时 `notify()`,把敏感于这些事件的 process 标记为下一轮 evaluate 的 runnable。
4. 如果第 3 步产生了新的 runnable process,**仿真时间不前进**,回到第 1 步——这就是下一个 delta(delta count +1,`sc_time_stamp()` 不变)。

这套机制保证了同一仿真时刻上多个并发 process 之间的确定性行为,和 Verilog 里非阻塞赋值(`<=`)"先算后赋"的语义是同一个思路。

## sc_start() 之后仿真如何推进

1. **初始化阶段**(只发生一次):每个 process(除非 `dont_initialize()`)先执行一次。
2. **Evaluate/Update/Delta-notify 循环**:在当前时间点反复迭代,直到没有更多 delta 事件、也没有更多 runnable process。
3. **时间推进**:当前时刻耗尽后,内核查看 timed notification 队列(来自 `wait(sc_time)`/`notify(sc_time)`),把 `sc_time_stamp()` 推进到最近的下一个事件时间点,唤醒对应 process,再跳回第 2 步。
4. **终止**:`sc_start()` 不带参数时,一直跑到彻底没有 pending 事件为止才返回;带时长参数则跑到那个时间点强制暂停,之后可以再次 `sc_start()` 续跑。

process 变成 runnable 的三种途径:初始化阶段自动跑一遍、静态敏感性事件被 notify、自己 `wait(...)` 声明的动态条件被满足。`SC_METHOD` 变 runnable 意味着重新从头执行;`SC_THREAD`/`SC_CTHREAD` 变 runnable 意味着从上次 `wait()` 之后那一行恢复执行(栈和局部变量都还在)。
