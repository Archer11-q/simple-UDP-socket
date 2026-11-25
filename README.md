# simple-UDP-socket

一个极简的 UDP 通信 Demo（Windows / C++17 / WinSock），用三种形态演示 UDP 的基本用法与特性。

## 功能

| 模式 | 行为 |
|---|---|
| `send` | 创建 socket → `sendto` → 关闭，一次性单向发送 |
| `recv` | `bind` 固定端口 → 循环 `recvfrom` → 打印来源地址与内容 |
| `chat` | 同一进程既能收也能发，两个终端互发消息 |

另外两条必要机制：

- **接收超时 300 ms**：`SO_RCVTIMEO`，避免接收线程永久阻塞、退出时无响应
- **正常退出**：输入 `exit` 或按 `Ctrl+C`，统一走"置标志 → join → closesocket"路径

## 构建

### 前置条件

| 依赖 | 说明 |
|---|---|
| Visual Studio 2022 | 需安装「使用 C++ 的桌面开发」工作负载 |
| CMake ≥ 3.21 | VS 自带一份，无需单独安装 |
| Windows SDK | 随 C++ 工作负载一起安装 |

> 注意：CMake 安装时通常不会写入系统 PATH，普通终端里直接敲 `cmake` 可能提示找不到命令。下面两种方式任选。

### 方式一：VS 开发者终端（推荐）

开始菜单搜索 **Developer PowerShell for VS 2022**，进入后直接执行：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

### 方式二：普通终端，用 VS 自带的 CMake

```powershell
$cmake = "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build -G "Visual Studio 17 2022" -A x64
& $cmake --build build --config Release
```

VS 装在非默认盘（例如 `D:\Program Files\...`）时，把路径前缀换成实际位置。

### 产物

```
build\Release\udpdemo.exe
```

清理构建：删除 `build\` 目录即可，该目录已在 `.gitignore` 中。

## 用法

以下命令中的 `udpdemo` 均指 `build\Release\udpdemo.exe`。

```
udpdemo send <ip> <port> <message...>              发送一条消息后退出
udpdemo recv <port>                                只接收，exit 或 Ctrl+C 退出
udpdemo chat <local-port> <peer-ip> <peer-port>    收发一体
```

同机演示（开两个终端）：

```powershell
# 终端 A
udpdemo recv 37020

# 终端 B
udpdemo send 127.0.0.1 37020 hello udp
```

收发一体：

```powershell
# 终端 A
udpdemo chat 37020 127.0.0.1 37021

# 终端 B
udpdemo chat 37021 127.0.0.1 37020
```

`chat` 模式下直接输入文字回车即发送，输入 `exit` 退出。

## 输出示例

接收：

```
[16:40:12] 127.0.0.1:52134  (9 字节)  hello udp
```

发送：

```
[发送] -> 127.0.0.1:37020  (9 字节)
```

错误：

```
[错误] bind 失败: 端口 37020  (WSA 错误 10048)
       该端口已被占用，请换一个端口
```

## 关于 UDP 的几点说明

- **不保证送达**：`sendto` 返回成功只代表交给了内核，对方是否收到无从得知
- **不保证顺序、可能重复**：Demo 不做任何排序或去重
- **无粘包**：一次 `recvfrom` 就是一条完整消息，不存在 TCP 的字节流边界问题
- **单条上限 1024 字节**：超过 MTU 会触发 IP 分片，丢任意一片整个包就丢了
- **首次运行需允许防火墙**，否则跨机收不到包（本机回环不受影响）

## 目录结构

```
.
├─ CMakeLists.txt
├─ README.md
└─ src\
   └─ main.cpp
```
