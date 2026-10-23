#include "server/pet_push_server.h"

// winsock2.h 必须先于 windows.h（否则 ws2tcpip.h C2065 SourceCount）：
// 本 .cpp 第一行已 include 本头（零 Windows 依赖），随后显式先 winsock2。
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <random>

#include "ipc/frame.h"
#include "ipc/protocol.h"
#include "log/log.h"

namespace ime::server {
namespace {

constexpr const char* kHelloAckJson =
    "{\"v\":1,\"type\":2,\"protocol_version\":1,\"ime_ready\":1}";

// 端口文件路径：%LOCALAPPDATA%\rime_beautify\runtime\ime.port.json
std::wstring PortFilePath() {
  wchar_t buf[MAX_PATH] = {};
  if (::GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH) == 0) {
    return L"";
  }
  return std::wstring(buf) + L"\\rime_beautify\\runtime\\ime.port.json";
}

// 极简 JSON 取整型（协议固定字段；非法返回 -1）
long long JsonInt(const std::string& s, const char* key) {
  const std::string k = std::string("\"") + key + "\":";
  std::size_t p = s.find(k);
  if (p == std::string::npos) {
    return -1;
  }
  p += k.size();
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) {
    ++p;
  }
  long long v = 0;
  bool neg = false;
  if (p < s.size() && s[p] == '-') {
    neg = true;
    ++p;
  }
  while (p < s.size() && std::isdigit(static_cast<unsigned char>(s[p]))) {
    v = v * 10 + (s[p] - '0');
    ++p;
  }
  return neg ? -v : v;
}

// 极简 JSON 取字符串
std::string JsonStr(const std::string& s, const char* key) {
  const std::string k = std::string("\"") + key + "\":\"";
  std::size_t p = s.find(k);
  if (p == std::string::npos) {
    return {};
  }
  p += k.size();
  const std::size_t e = s.find('"', p);
  if (e == std::string::npos) {
    return {};
  }
  return s.substr(p, e - p);
}

// 读满 n 字节（循环 recv）；返回 false = 连接关闭/错误
bool RecvAll(SOCKET s, std::uint8_t* buf, std::size_t n) {
  std::size_t got = 0;
  while (got < n) {
    const int r = ::recv(s, reinterpret_cast<char*>(buf) + got,
                         static_cast<int>(n - got), 0);
    if (r <= 0) {
      return false;
    }
    got += static_cast<std::size_t>(r);
  }
  return true;
}

// 线程参数：self + 新连接 socket（AcceptLoop 分配，ClientThread 释放）
struct ClientArgs {
  PetPushServer* self = nullptr;
  SOCKET s = INVALID_SOCKET;
};

}  // namespace

bool PetPushServer::Start() {
  WSADATA wsa{};
  if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    ::ime::LogF("[pet-push] FAIL: WSAStartup err=%d\n", ::WSAGetLastError());
    return false;
  }
  const SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (ls == INVALID_SOCKET) {
    ::ime::LogF("[pet-push] FAIL: socket err=%d\n", ::WSAGetLastError());
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;  // 随机端口
  if (::bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
      SOCKET_ERROR) {
    ::ime::LogF("[pet-push] FAIL: bind err=%d\n", ::WSAGetLastError());
    ::closesocket(ls);
    return false;
  }
  int len = sizeof(addr);
  ::getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &len);
  const std::uint16_t port = ntohs(addr.sin_port);
  if (::listen(ls, SOMAXCONN) == SOCKET_ERROR) {
    ::ime::LogF("[pet-push] FAIL: listen err=%d\n", ::WSAGetLastError());
    ::closesocket(ls);
    return false;
  }
  listen_sock_ = static_cast<std::uintptr_t>(ls);

  // 随机 token（32 hex）
  {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    char hex[33] = {};
    const char* table = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
      hex[i] = table[gen() & 0xF];
    }
    token_.assign(hex, 32);
  }

  // 写端口文件（桌宠读取后连接）
  const std::wstring pf = PortFilePath();
  if (!pf.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(pf).parent_path(),
                                        ec);
    rime_beautify::ipc::PortFile port_file{};
    port_file.protocol_version = 1;
    port_file.port = port;
    port_file.token = token_;
    const std::string json = rime_beautify::ipc::PortFileToJson(port_file);
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, pf.c_str(), L"wb") == 0 && f != nullptr) {
      std::fwrite(json.data(), 1, json.size(), f);
      std::fclose(f);
    }
  }

  running_ = true;
  const HANDLE h = ::CreateThread(nullptr, 0, AcceptLoop, this, 0, nullptr);
  accept_thread_ = reinterpret_cast<std::uintptr_t>(h);
  ::ime::LogF("[pet-push] OK: 桌宠推送端口 %u token=%s%s\n", port,
              token_.c_str(), pf.empty() ? "（端口文件路径不可用）" : "");
  return true;
}

void PetPushServer::Stop() {
  running_ = false;
  if (listen_sock_ != 0) {
    ::closesocket(static_cast<SOCKET>(listen_sock_));  // 唤醒阻塞中的 accept
    listen_sock_ = 0;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::uintptr_t s : clients_) {
      ::closesocket(static_cast<SOCKET>(s));
    }
    clients_.clear();
  }
  if (accept_thread_ != 0) {
    ::WaitForSingleObject(reinterpret_cast<HANDLE>(accept_thread_), 2000);
    ::CloseHandle(reinterpret_cast<HANDLE>(accept_thread_));
    accept_thread_ = 0;
  }
}

void PetPushServer::SendKeyEvent(std::uint32_t vk, std::uint32_t scan,
                                 bool make) {
  if (!running_ || clients_.empty()) {
    return;  // best-effort：无桌宠连接时静默丢弃
  }
  const std::uint64_t ts = ::GetTickCount64();
  std::string json = "{\"v\":1,\"type\":3,\"vk\":";
  json += std::to_string(vk);
  json += ",\"scan\":";
  json += std::to_string(scan);
  json += ",\"make\":";
  json += make ? "1" : "0";
  json += ",\"ts\":";
  json += std::to_string(ts);
  json += "}";
  Broadcast(rime_beautify::ipc::EncodeFrame(json));
}

void PetPushServer::SendPetVisible(bool visible) {
  if (!running_ || clients_.empty()) {
    return;  // best-effort：无桌宠连接时静默丢弃
  }
  const std::uint64_t ts = ::GetTickCount64();
  std::string json = "{\"v\":1,\"type\":19,\"visible\":";
  json += visible ? "1" : "0";
  json += ",\"ts\":";
  json += std::to_string(ts);
  json += "}";
  Broadcast(rime_beautify::ipc::EncodeFrame(json));
}

void PetPushServer::Broadcast(const std::vector<std::uint8_t>& frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = clients_.begin(); it != clients_.end();) {
    const SOCKET s = static_cast<SOCKET>(*it);
    const int r = ::send(s, reinterpret_cast<const char*>(frame.data()),
                         static_cast<int>(frame.size()), 0);
    if (r == SOCKET_ERROR) {  // 对端关闭/缓冲满 → 移除
      ::closesocket(s);
      it = clients_.erase(it);
    } else {
      ++it;
    }
  }
}

void PetPushServer::RemoveClient(std::uintptr_t s) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = clients_.begin(); it != clients_.end(); ++it) {
    if (*it == s) {
      ::closesocket(static_cast<SOCKET>(s));
      clients_.erase(it);
      return;
    }
  }
}

unsigned long __stdcall PetPushServer::AcceptLoop(void* param) {
  auto* self = static_cast<PetPushServer*>(param);
  while (self->running_) {
    const SOCKET ls =
        self->listen_sock_ != 0 ? static_cast<SOCKET>(self->listen_sock_)
                                : INVALID_SOCKET;
    SOCKET cs = ::accept(ls, nullptr, nullptr);
    if (cs == INVALID_SOCKET) {
      if (!self->running_) {
        break;
      }
      continue;
    }
    auto* args = new ClientArgs{self, cs};
    HANDLE h = ::CreateThread(nullptr, 0, ClientThread, args, 0, nullptr);
    if (h != nullptr) {
      ::CloseHandle(h);
    } else {
      delete args;
      ::closesocket(cs);
    }
  }
  return 0;
}

unsigned long __stdcall PetPushServer::ClientThread(void* param) {
  auto* args = static_cast<ClientArgs*>(param);
  PetPushServer* self = args->self;
  const SOCKET s = args->s;
  delete args;

  // 1) 收 hello 帧（第一帧必须是鉴权）
  std::uint8_t hdr[4] = {};
  if (!RecvAll(s, hdr, 4)) {
    ::closesocket(s);
    return 0;
  }
  std::uint32_t len = (static_cast<std::uint32_t>(hdr[0]) << 24) |
                      (static_cast<std::uint32_t>(hdr[1]) << 16) |
                      (static_cast<std::uint32_t>(hdr[2]) << 8) |
                      static_cast<std::uint32_t>(hdr[3]);
  if (len == 0 || len > (1u << 20)) {
    ::closesocket(s);
    return 0;
  }
  std::vector<std::uint8_t> buf(len);
  if (!RecvAll(s, buf.data(), len)) {
    ::closesocket(s);
    return 0;
  }
  std::string json(reinterpret_cast<const char*>(buf.data()), len);
  const long long v = JsonInt(json, "v");
  const long long type = JsonInt(json, "type");
  const std::string token = JsonStr(json, "token");
  if (type != 1 || v != 1 || token != self->token_) {
    ::ime::LogF("[pet-push] 鉴权失败断开（防伪造按键事件）\n");
    ::closesocket(s);
    return 0;
  }

  // 2) 回 hello_ack
  const auto ack = rime_beautify::ipc::EncodeFrame(kHelloAckJson);
  ::send(s, reinterpret_cast<const char*>(ack.data()),
         static_cast<int>(ack.size()), 0);
  {
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->clients_.push_back(static_cast<std::uintptr_t>(s));
  }
  ::ime::LogF("[pet-push] 桌宠已连接，鉴权通过\n");

  // 3) 收帧循环（ping/pong 保活；bye 退出；未知类型忽略）
  while (self->running_) {
    if (!RecvAll(s, hdr, 4)) {
      break;
    }
    len = (static_cast<std::uint32_t>(hdr[0]) << 24) |
          (static_cast<std::uint32_t>(hdr[1]) << 16) |
          (static_cast<std::uint32_t>(hdr[2]) << 8) |
          static_cast<std::uint32_t>(hdr[3]);
    if (len > (1u << 20)) {
      break;
    }
    if (len == 0) {
      continue;
    }
    buf.resize(len);
    if (!RecvAll(s, buf.data(), len)) {
      break;
    }
    json.assign(reinterpret_cast<const char*>(buf.data()), len);
    if (JsonInt(json, "type") == 6) {  // bye（用户右键退出桌宠 → 通知 server 记住）
      if (self->on_pet_bye_) {
        self->on_pet_bye_();
      }
      break;
    }
  }
  self->RemoveClient(static_cast<std::uintptr_t>(s));
  ::ime::LogF("[pet-push] 桌宠连接断开\n");
  return 0;
}

}  // namespace ime::server
