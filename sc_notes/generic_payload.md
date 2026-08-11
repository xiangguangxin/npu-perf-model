# TLM Generic Payload

## 字段一览

| 属性 | 类型 | 含义 |
|---|---|---|
| `command` | `tlm_command` | `TLM_READ_COMMAND` / `TLM_WRITE_COMMAND` / `TLM_IGNORE_COMMAND` |
| `address` | `sc_dt::uint64` | 目标地址 |
| `data_ptr` | `unsigned char*` | 数据缓冲区指针(★由 initiator 分配) |
| `data_length` | `unsigned int` | 数据字节数 |
| `byte_enable_ptr` | `unsigned char*` | 字节使能掩码(可选,0xFF 有效/0x00 无效) |
| `byte_enable_length` | `unsigned int` | 掩码长度 |
| `streaming_width` | `unsigned int` | 流式传输宽度(地址不递增的 FIFO 型访问) |
| `response_status` | `tlm_response_status` | 响应状态(★由 target 填写) |
| `dmi_allowed` | `bool` | target 提示"此区域可用 DMI 加速" |

## 示例:发起一次 write transaction

```cpp
tlm::tlm_generic_payload trans;
unsigned char buf[4];
sc_time delay = SC_ZERO_TIME;

trans.set_command(tlm::TLM_WRITE_COMMAND);
trans.set_address(0x1000);
trans.set_data_ptr(buf);
trans.set_data_length(4);
trans.set_streaming_width(4);              // 等于 data_length 表示普通访问
trans.set_byte_enable_ptr(nullptr);        // 不用字节使能
trans.set_dmi_allowed(false);
trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);  // ★初始必须设这个

socket->b_transport(trans, delay);

if (trans.is_response_error())
    SC_REPORT_ERROR("CPU", trans.get_response_string());
```

## 要点

- `data_ptr` 由 **initiator** 分配并持有,target 只读/写这块内存,不负责其生命周期。
- `response_status` 必须由 initiator 先初始化成 `TLM_INCOMPLETE_RESPONSE`,再由 **target** 在处理完成后改写成最终状态(如 `TLM_OK_RESPONSE`);调用方靠 `is_response_error()` 判断成败。
- `dmi_allowed` 只是 initiator 给 target 的一个"提示"——表示自己愿意在这次调用里顺带取 DMI 指针,真正是否给、给哪块地址范围,由 target 决定。

## 内存管理(高频、易挂)

- **谁分配、谁释放**:LT 下通常 initiator 在栈上创建/复用一个 payload 就够了;但 AT 下事务的生命周期跨越多个阶段、多个组件,必须用引用计数——`acquire()` / `release()`,配合 memory manager(`tlm_mm_interface`)管理。
- **追问**:"为什么 AT 模式下不能在 `nb_transport_fw` 返回后就释放 payload?"——因为 target 可能还持有这个指针,事务要到 `END_RESP` 才算真正结束。
- **payload 池化复用**:实际工程里为什么要做 payload pool——避免频繁 `new`/`delete` 拖慢仿真。

## Extension 机制

- `set_extension` / `get_extension`:基于类型索引的数组,给 payload 挂载协议未定义的额外信息。
- **ignorable extension vs 需要新协议类型**:什么样的扩展可以让不认识它的组件安全忽略,什么时候必须定义新的 protocol traits class(不能再叫 base protocol)——这直接关联互操作性。
- 自定义协议时,`tlm_generic_payload` + extension 和完全自定义 payload 之间的取舍。

## AT / 非阻塞传输的完整细节(核心中的核心)

- **四阶段握手的完整时序**:`BEGIN_REQ → END_REQ → BEGIN_RESP → END_RESP`,`BEGIN_REQ`/`END_RESP` 由 initiator 发起(fw 路径),`END_REQ`/`BEGIN_RESP` 由 target 发起(bw 路径)。
- **三种返回值的语义**:
  - `TLM_ACCEPTED`——纯接受,后续状态推进都靠反向(bw)调用完成;
  - `TLM_UPDATED`——被调用方直接把 phase 推进了(跳过了本该异步到来的那次 bw/fw 调用);
  - `TLM_COMPLETED`——事务提前一步直接结束。
  - **追问**:target 收到 `BEGIN_REQ` 直接返回 `TLM_COMPLETED` 意味着什么、什么时候合法?(意味着这次访问在逻辑上瞬间/同步完成,常见于 timing 可以完全用 `delay` 参数表达、不需要真正走后续三个 phase 的场景)
- **timing annotation**:`sc_time& delay` 参数的含义——"这个调用逻辑上发生在当前时刻 + delay",接收方可以选择立即处理(把 delay 记账)或放进 PEQ,等到那个时刻再处理。
- **请求/响应互斥规则(exclusion rules)**:同一 socket 上,上一个事务的 `END_REQ` 到达之前不能发下一个 `BEGIN_REQ`——这是 base protocol 对流控的规定,常考"TLM 2.0 如何建模总线的 outstanding 事务和背压"。
- **PEQ(payload event queue)**:`peq_with_cb_and_phase` / `peq_with_get` 的用途——把带时间标注的事务按时间序排队处理,是写 AT 模型的标配工具。

## LT 的深入点

- **`b_transport` 中 wait vs 时间标注**:两种风格——每个组件自己 `wait(delay)`(慢但简单),或一路累加 `delay` 不 `wait`、由 initiator 统一消化(快,这就是时间解耦的基础)。
- **Temporal decoupling + quantum 的完整机制**:`tlm_quantumkeeper` 的 `inc()` / `need_sync()` / `sync()`,global quantum 如何设置,quantum 大小的权衡——越大仿真越快,但时序精度和组件间交互的正确性越差。
  - **追问**:"quantum 设太大会出什么问题?"——可能错过中断、共享资源竞争建模失真。
- **LT 和 AT 混合系统**:b/nb 之间怎么桥接(simple socket 自带自动转换 adapter),转换过程会丢失哪些信息(比如精确的 phase 时序退化成单次调用 + delay)。
