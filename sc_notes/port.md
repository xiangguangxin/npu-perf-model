# SystemC Port / Export / Interface

## 类比

- **port 是港口**,类比插头,为使用方。
- **export 是出口**,类比插座,为提供方。

## interface

`interface` 是一个抽象类,继承自 `sc_interface`,不继承 `sc_object`;它定义了一组纯虚函数(即还没有实现的函数),通道(channel)会实现这个接口中的函数。

```cpp
struct my_if : public sc_interface {
    virtual void write(int) = 0;
    virtual int read() = 0;
};
```

这个接口定义了两项服务:`write(int)` 和 `read()`,但并没有实现。

## sc_port

```cpp
template <class IF>
class sc_port {
    IF* m_interface;   // 核心:就存了个接口指针
public:
    // ① 直接绑到实现接口的对象/channel
    void bind(IF& if_) { m_interface = &if_; }

    // ② 绑到一个 export:把 export 里存的指针取过来
    void bind(sc_export<IF>& ex) {
        m_interface = ex.get_interface();   // ← 正好用到 export 的 get_interface()
    }

    IF* operator->() {
        return m_interface;   // port->write() 实际是 m_interface->write()
    }
};
```

`m_interface` 就是靠 `bind()` 填上的——这正是顶层那句 `port.bind(export)`。和 `sc_export` 对称起来看:

- **`export.bind(*this)`**:把"实现"的地址**存进 export**;
- **`port.bind(export)`**:再把这个地址从 export **取出来存进 port**(经 `get_interface()`)。

于是 `port->write()` 用的 `m_interface`,是顺着 ②→`get_interface()`→export 里存的 `*this` 一路传过来的。

> **真实内核是延迟解析,不是当场拷指针**:顶层 `port.bind(export)` 执行时,那个 export 可能还没 `bind(*this)`(模块构造顺序不保证)。所以内核先把绑定关系记下,等 **elaboration 结束的 complete-binding 阶段**才统一把最终接口指针一路穿透赋值。上面的"当场赋值"只是简化示意。

## sc_export

```cpp
template <class IF>
class sc_export {
    IF* m_interface;
public:
    void bind(IF& if_) {
        m_interface = &if_;    // exp.bind(*this) 就是把 Server 自己的地址存进来
    }

    IF* get_interface() {
        return m_interface;    // 别人来问时,把这个指针交出去
    }
};
```

## port 中用了重载

C++ 的 `operator->` 有个特殊的语言规则:重载函数返回一个指针后,编译器会自动对这个返回的指针再应用一次原生 `->`。所以:

```cpp
port->write(42);
// 编译器展开为:
port.operator->()->write(42);
//     ↑返回 IF*      ↑对返回的指针做真正的成员调用
```

这和智能指针 `std::shared_ptr`、`std::unique_ptr` 是同一套机制——它们也是对象,靠重载 `operator->` 来"伪装"成指针用。

## 完整示例:Server 通过 export 暴露服务

```cpp
// 服务提供方:继承 my_if, 实现 write, 通过 export 暴露
class Server : public sc_module, public my_if {
public:
    sc_export<my_if> exp;

    SC_CTOR(Server) {
        exp.bind(*this);   // ★ 必须有这一步!把 export 绑到自己
    }

    virtual void write(int v) override {
        cout << "Server 收到: " << v << endl;
    }
};
```

`exp.bind(*this)` 里,由于 `Server` 继承了 `my_if`,`*this` 可以隐式向上转型(upcast)成 `my_if&`,于是 export 里存下的就是指向 `Server` 对象的 `my_if*` 指针。
