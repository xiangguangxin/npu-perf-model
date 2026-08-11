# TLM Socket = port + export 的对称打包

## 基本结构

### 概念直觉：一个 port + 一个 export

先建立直觉——每个 socket 都"含"一条主动路(port)和一条被动路(export):

```cpp
// 概念示意（注意：真实实现并非两个都是成员，见下）
class tlm_initiator_socket {
    sc_port  <tlm_fw_transport_if>  fw_port;   // 我要调用对方的前向接口
    sc_export<tlm_bw_transport_if>  bw_export; // 我提供反向接口给对方回调
};

class tlm_target_socket {
    sc_export<tlm_fw_transport_if>  fw_export; // 我提供前向接口(b_transport 在这)
    sc_port  <tlm_bw_transport_if>  bw_port;   // 我要回调对方的反向接口
};
```

### 真实实现：不对称——主动路是基类，被动路是成员

看 Accellera 源码，上面的"两个成员"其实是**不对称**的：**主动调用的那条路(port)是继承来的基类(socket IS-A port)，被动接受回调的那条路(export)才是成员(HAS-A)**。

```
tlm_initiator_socket
   └─▶ tlm_base_initiator_socket<BUSWIDTH, FW_IF, BW_IF, N, POL>
          ├─▶ public sc_port<FW_IF, N, POL>          ← 继承(IS-A)：主动的前向 port
          ├─▶ public tlm_base_initiator_socket_b<…>  ┐ 两层纯接口基类
          │        └─▶ public tlm_base_socket_if      ┘ (get_base_port / get_base_export)
          └─  成员 sc_export<BW_IF> m_export;         ← 组合(HAS-A)：被动的反向 export
```

target socket 正好镜像：**IS-A `sc_export`**(前向被调，b_transport 在这) + **HAS-A `sc_port`**(反向主调回 initiator)。

| | 前向(forward) | 反向(backward) |
|-----------|----------------------|----------------------|
| initiator | **基类 sc_port**(主动) | 成员 sc_export(被动) |
| target    | **基类 sc_export**(被动) | 成员 sc_port(主动) |

一句话记忆：**"我主动打出去的那条路，做成基类；别人回调我的那条路，做成成员。"** 谁主动，谁就是基类。

> 为什么这样设计：主动路做成基类，`socket->b_transport(...)` 这种 `operator->` 语法才能直接复用 sc_port 的实现（见下"两个细节"），层次化绑定时也能像普通 port 一样往外引。被动路只需被持有，用成员 export 即可。

```
Initiator                                Target
┌─────────────────┐                   ┌─────────────────┐
│ init_socket      │                   │ targ_socket     │
│                  │   forward path    │                 │
│  fw_port ────────┼──────────────────►│─── fw_export    │
│                  │                   │   (b_transport) │
│  bw_export ◄─────┼───────────────────┼─── bw_port      │
│ (nb_transport_bw)│   backward path   │                 │
└─────────────────┘                   └─────────────────┘
```

## 一次 bind,两条链

顶层写:

```cpp
cpu.init_socket.bind(mem.targ_socket);
```

socket 的 `bind()` 内部实际做了两件事:

```cpp
// tlm_initiator_socket::bind(tlm_target_socket& s) 简化逻辑
void bind(tlm_target_socket& s) {
    this->fw_port.bind(s.fw_export);   // 前向:我的 port → 你的 export
    s.bw_port.bind(this->bw_export);   // 反向:你的 port → 我的 export
}
```

## 原生 socket 的用法

```cpp
// Target 必须实现前向接口
struct Memory : sc_module, tlm::tlm_fw_transport_if<> {
    tlm::tlm_target_socket<> socket;

    SC_CTOR(Memory) : socket("socket") {
        socket.bind(*this);          // ★ 把 socket 内部的 fw_export 绑到自己
    }

    // 前向接口的实现
    virtual void b_transport(tlm::tlm_generic_payload& trans,
                             sc_time& delay) override {
        // 处理读写...
    }
    virtual tlm::tlm_sync_enum nb_transport_fw(...) override { ... }
    virtual bool get_direct_mem_ptr(...) override { ... }
    virtual unsigned int transport_dbg(...) override { ... }
};

// Initiator 必须实现反向接口
struct Cpu : sc_module, tlm::tlm_bw_transport_if<> {
    tlm::tlm_initiator_socket<> socket;

    SC_CTOR(Cpu) : socket("socket") {
        socket.bind(*this);          // ★ 把 socket 内部的 bw_export 绑到自己
        SC_THREAD(run);
    }

    void run() {
        tlm::tlm_generic_payload trans;
        sc_time delay = SC_ZERO_TIME;
        // ...填 payload...
        socket->b_transport(trans, delay);   // 通过 socket 的 fw_port 调过去
    }

    // 反向接口的实现
    virtual tlm::tlm_sync_enum nb_transport_bw(...) override { ... }
    virtual void invalidate_direct_mem_ptr(...) override { ... }
};
```

注意两个细节,都能用前面(port/export 笔记里)的知识解释:

- `socket.bind(*this)`:又是那次隐式 upcast,把自己以接口指针的身份存进 socket 内部的 export。
- `socket->b_transport(...)`:因为 socket **就继承自 sc_port**(见上"真实实现"),这里用的**直接是 sc_port 自己的 `operator->`**——不是"重载 operator-> 再转发给某个成员 fw_port",而是字面意义上同一个 `operator->`。所以它和 `port->write(42)` 不只是"同一个机制",根本就是同一份 sc_port 实现在跑。

## simple socket:省掉样板代码

原生 socket 有个恼人之处:哪怕你只用阻塞传输,也必须把 `tlm_fw_transport_if` 的全部 4 个纯虚函数都实现一遍(`nb_transport_fw`、DMI、debug 都得写空壳)。所以实践中大家几乎都用 utilities 里的便捷 socket:

```cpp
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

struct Memory : sc_module {                    // 不用继承接口了!
    tlm_utils::simple_target_socket<Memory> socket;

    SC_CTOR(Memory) : socket("socket") {
        // 只注册你关心的回调
        socket.register_b_transport(this, &Memory::b_transport);
    }
    void b_transport(tlm::tlm_generic_payload& trans, sc_time& delay) { ... }
};
```

simple socket 内部自带一个实现了完整接口的小对象(未注册的方法给出默认行为),它替你完成了"继承接口 + `bind(*this)`"那套样板代码。原理没变,只是封装得更省事。

## 补充几点

- **方向约定**:initiator socket 只能绑 target socket(或经由 interconnect 模块中转)。总线/路由器这类中间模块两边都有——面向 CPU 一侧放 `target_socket`,面向 Memory 一侧放 `initiator_socket`,收到事务后转发,正如最开始例子里那个既有 export 又有 port 的 Middle 模块,只是双向版。
- **层次化绑定**:socket 同样支持往外引,父模块的 initiator socket 绑子模块的 initiator socket(同类相绑传递到边界),内核在 complete binding 时照样把最终接口指针一路穿透下去。
- **模板参数**:`tlm_initiator_socket<BUSWIDTH, TYPES>` 可指定总线位宽(默认 32)和协议类型(默认 `tlm_base_protocol_types`,即 generic payload + phase),两端要匹配才能绑定。

## 一句话总结

socket = port + export 的打包(initiator/target 互为镜像;实现上主动路是继承的基类、被动路是成员,见"基本结构"),让双向的 TLM 调用只需一次绑定;它没有引入任何新机制,底层仍是你已经理解的那三件事——隐式 upcast 存指针、elaboration 传指针、虚函数完成分发。
