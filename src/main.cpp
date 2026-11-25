// udpdemo.cpp — UDP 通信 Demo（发送 / 接收 / 收发一体）
//
// 用法:
//   udpdemo send <ip> <port> <message...>              发送一条消息后退出
//   udpdemo recv <port>                                只接收，exit 或 Ctrl+C 退出
//   udpdemo chat <local-port> <peer-ip> <peer-port>    收发一体
//
// 设计要点见 docs/设计文档.md：接收超时 300ms、退出标志驱动、先 join 再关 socket。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>   // SIO_UDP_CONNRESET 定义在此（不是 mstcpip.h）
#include <windows.h>
#include <conio.h>
#include <io.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#pragma comment(lib, "ws2_32.lib")

namespace {

// ---------------------------------------------------------------- 常量

constexpr int kRecvTimeoutMs = 300;   // SO_RCVTIMEO：退出响应上限 + 轮询节拍
constexpr int kStdinPollMs = 30;      // 主线程轮询 stdin 的间隔
constexpr std::size_t kMaxPayload = 1024;   // 单条消息上限，避免 IP 分片
constexpr int kRecvBufferSize = 2048;       // 收包缓冲区

std::atomic<bool> g_stop{false};

// ---------------------------------------------------------------- RAII

// Winsock 初始化/清理，构造成功则析构时必定 WSACleanup
struct WsaInit {
    bool ok = false;
    WsaInit() {
        WSADATA data{};
        ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
    }
    ~WsaInit() {
        if (ok) WSACleanup();
    }
    WsaInit(const WsaInit&) = delete;
    WsaInit& operator=(const WsaInit&) = delete;
};

// socket 句柄 RAII，避免任何提前 return 造成泄漏
struct Socket {
    SOCKET s = INVALID_SOCKET;

    Socket() = default;
    explicit Socket(SOCKET handle) : s(handle) {}
    ~Socket() { close(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool valid() const { return s != INVALID_SOCKET; }

    void close() {
        if (s != INVALID_SOCKET) {
            closesocket(s);
            s = INVALID_SOCKET;
        }
    }
};

// ---------------------------------------------------------------- 工具

// Ctrl+C 处理：只置标志。处理函数跑在系统线程，
// 里面打印/分配内存/关 socket 都可能导致死锁。
BOOL WINAPI ctrlHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        g_stop.store(true);
        return TRUE;
    default:
        return FALSE;
    }
}

std::string timeStr() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[16]{};
    std::snprintf(buf, sizeof buf, "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    return std::string(buf);
}

std::string endpointStr(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN]{};
    InetNtopA(AF_INET, const_cast<in_addr*>(&addr.sin_addr), ip, sizeof ip);
    char buf[64]{};
    std::snprintf(buf, sizeof buf, "%s:%u", ip, ntohs(addr.sin_port));
    return std::string(buf);
}

bool parsePort(const char* text, uint16_t& out) {
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 65535) return false;
    out = static_cast<uint16_t>(value);
    return true;
}

bool resolveIpv4(const char* text, sockaddr_in& out) {
    out = sockaddr_in{};
    out.sin_family = AF_INET;
    return InetPtonA(AF_INET, text, &out.sin_addr) == 1;
}

void setRecvTimeout(SOCKET s, int milliseconds) {
    DWORD timeout = static_cast<DWORD>(milliseconds);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof timeout);
}

// 关闭 ICMP 端口不可达导致的 WSAECONNRESET，避免 recvfrom 被"打醒"
void disableConnReset(SOCKET s) {
    BOOL off = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off,
             nullptr, 0, &returned, nullptr, nullptr);
}

void usage() {
    std::printf(
        "用法:\n"
        "  udpdemo send <ip> <port> <message...>              发送一条消息后退出\n"
        "  udpdemo recv <port>                                只接收，exit 或 Ctrl+C 退出\n"
        "  udpdemo chat <local-port> <peer-ip> <peer-port>    收发一体\n"
        "\n"
        "示例:\n"
        "  udpdemo recv 37020\n"
        "  udpdemo send 127.0.0.1 37020 hello udp\n"
        "  udpdemo chat 37020 127.0.0.1 37021\n");
}

// ---------------------------------------------------------------- 收发

bool sendDatagram(SOCKET s, const sockaddr_in& to, const std::string& message) {
    if (message.size() > kMaxPayload) {
        std::printf("[错误] 消息过长: %zu 字节，上限 %zu\n", message.size(), kMaxPayload);
        return false;
    }
    int sent = sendto(s, message.data(), static_cast<int>(message.size()), 0,
                      reinterpret_cast<const sockaddr*>(&to),
                      static_cast<int>(sizeof to));
    if (sent == SOCKET_ERROR) {
        std::printf("[错误] sendto 失败: %d\n", WSAGetLastError());
        return false;
    }
    std::printf("[发送] -> %s  (%d 字节)\n", endpointStr(to).c_str(), sent);
    std::fflush(stdout);
    return true;
}

// 接收线程：超时不是错误，只是回来检查一次退出标志
void recvLoop(SOCKET s) {
    std::string buffer(static_cast<std::size_t>(kRecvBufferSize), '\0');

    while (!g_stop.load()) {
        sockaddr_in from{};
        int fromLen = static_cast<int>(sizeof from);

        int received = recvfrom(s, buffer.data(), kRecvBufferSize, 0,
                                reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAETIMEDOUT) continue;      // 正常节拍
            if (error == WSAECONNRESET) continue;     // ICMP 端口不可达
            if (error == WSAEMSGSIZE) {
                std::printf("[警告] 收到超长报文，已丢弃\n");
                std::fflush(stdout);
                continue;
            }
            if (!g_stop.load()) {
                std::printf("[错误] recvfrom 失败: %d\n", error);
                std::fflush(stdout);
            }
            break;
        }

        std::string text(buffer.data(), static_cast<std::size_t>(received));
        std::printf("[%s] %s  (%d 字节)  %s\n",
                    timeStr().c_str(), endpointStr(from).c_str(), received, text.c_str());
        std::fflush(stdout);
    }
}

// stdin 是否连着控制台。重定向/管道输入时 _kbhit 恒为 0，
// 必须退回阻塞 getline，否则永远读不到数据。
bool stdinIsConsole() {
    return _isatty(_fileno(stdin)) != 0;
}

// 归一化一行输入：
//   - 去掉行首 UTF-8 BOM（PowerShell 5.1 通过管道写原生程序时会附加 EF BB BF）
//   - 去掉行尾的 \r / \n（不同终端的换行习惯）
std::string normalizeLine(std::string line) {
    if (line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

// 主线程：轮询 stdin。不能用阻塞 getline，否则 Ctrl+C 后无法及时退出。
void stdinLoop(bool chat, const sockaddr_in& peer, SOCKET s) {
    std::string line;
    const bool console = stdinIsConsole();

    while (!g_stop.load()) {
        if (console && !_kbhit()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kStdinPollMs));
            continue;
        }

        if (!std::getline(std::cin, line)) {   // Ctrl+Z / 输入流结束
            g_stop.store(true);
            break;
        }
        line = normalizeLine(line);

        if (line == "exit" || line == "quit") {
            g_stop.store(true);
            break;
        }
        if (line.empty()) continue;

        if (chat) {
            sendDatagram(s, peer, line);
        } else {
            std::printf("[提示] recv 模式不发送，输入 exit 退出\n");
            std::fflush(stdout);
        }
        if (g_stop.load()) break;
    }
}

// ---------------------------------------------------------------- 模式

int senderMain(int argc, char** argv) {
    if (argc < 5) {
        usage();
        return 1;
    }

    sockaddr_in target{};
    uint16_t port = 0;
    if (!resolveIpv4(argv[2], target) || !parsePort(argv[3], port)) {
        std::printf("[错误] 目标地址或端口无效\n");
        return 1;
    }
    target.sin_port = htons(port);

    std::string message;
    for (int i = 4; i < argc; ++i) {
        if (i > 4) message += ' ';
        message += argv[i];
    }
    if (message.size() > kMaxPayload) {
        std::printf("[错误] 消息过长: %zu 字节，上限 %zu\n", message.size(), kMaxPayload);
        return 1;
    }

    Socket sock(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!sock.valid()) {
        std::printf("[错误] 创建 socket 失败: %d\n", WSAGetLastError());
        return 1;
    }

    return sendDatagram(sock.s, target, message) ? 0 : 1;
}

int receiverMain(int argc, char** argv) {
    const bool chat = (std::strcmp(argv[1], "chat") == 0);

    if (argc < 3) {
        usage();
        return 1;
    }

    uint16_t localPort = 0;
    if (!parsePort(argv[2], localPort)) {
        std::printf("[错误] 本地端口无效\n");
        return 1;
    }

    sockaddr_in peer{};
    if (chat) {
        if (argc < 5) {
            usage();
            return 1;
        }
        uint16_t peerPort = 0;
        if (!resolveIpv4(argv[3], peer) || !parsePort(argv[4], peerPort)) {
            std::printf("[错误] 对端地址或端口无效\n");
            return 1;
        }
        peer.sin_port = htons(peerPort);
    }

    Socket sock(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!sock.valid()) {
        std::printf("[错误] 创建 socket 失败: %d\n", WSAGetLastError());
        return 1;
    }

    setRecvTimeout(sock.s, kRecvTimeoutMs);
    disableConnReset(sock.s);

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(localPort);

    if (bind(sock.s, reinterpret_cast<sockaddr*>(&local), sizeof local) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        std::printf("[错误] bind 失败: 端口 %u  (WSA 错误 %d)\n", localPort, error);
        if (error == WSAEADDRINUSE) {
            std::printf("       该端口已被占用，请换一个端口\n");
        }
        return 1;
    }

    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    std::printf("已监听 0.0.0.0:%u\n", localPort);
    if (chat) {
        std::printf("对端 %s，输入内容回车发送，输入 exit 退出（Ctrl+C 亦可）\n",
                    endpointStr(peer).c_str());
    } else {
        std::printf("等待接收，输入 exit 退出（Ctrl+C 亦可）\n");
    }
    std::fflush(stdout);

    std::thread receiver(recvLoop, sock.s);
    stdinLoop(chat, peer, sock.s);

    g_stop.store(true);
    receiver.join();          // 先等线程退出，再让 Socket 析构关闭句柄
    std::printf("已退出\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc < 2) {
        usage();
        return 1;
    }

    WsaInit wsa;
    if (!wsa.ok) {
        std::printf("[错误] WSAStartup 失败: %d\n", WSAGetLastError());
        return 1;
    }

    const std::string mode = argv[1];
    if (mode == "send") return senderMain(argc, argv);
    if (mode == "recv") return receiverMain(argc, argv);
    if (mode == "chat") return receiverMain(argc, argv);

    usage();
    return 1;
}
