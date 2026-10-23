// 桌宠事件推送服务器（M3-4）：输入法服务进程 → 桌宠的按键事件通道。
// 协议 v1（docs/ipc-protocol.md）：
//   - TCP loopback 随机端口；IME 启动时把 {protocol_version, port, token}
//     写入 %LOCALAPPDATA%\rime_beautify\runtime\ime.port.json；
//   - 桌宠连接后先发 hello{token} 鉴权，校验失败立即断开；
//   - 帧 = 4 字节大端长度 + UTF-8 JSON（shared/ipc/frame.h EncodeFrame）；
//   - 消息：type=1 hello(桌宠→IME) / type=2 hello_ack(IME→桌宠) /
//           type=3 key_event(IME→桌宠, {vk,scan,make,ts}) / type=6 bye；
//           type=19 pet_visibility(IME→桌宠, {visible}) 输入法激活显隐；
//   - 断线由桌宠侧退避重连（1s→2s→4s…30s），本服务只管监听与广播。
//
// 本头文件刻意零 Windows 依赖（不 include winsock2.h/windows.h）：
// Windows 惯例 winsock2.h 必须先于 windows.h 出现（否则 ws2tcpip.h
// C2065 SourceCount），而本头会被已 include 过 windows.h 的 TU 引用。
// socket/HANDLE 句柄在头文件用 uintptr_t 存，仅在 .cpp 内转真实类型
// （.cpp 自己保证 winsock2.h 先于 windows.h）。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ime::server {

class PetPushServer {
 public:
  PetPushServer() = default;
  ~PetPushServer() { Stop(); }

  // 绑定 127.0.0.1 随机端口 + 写端口文件 + 启动 accept 线程。
  // 返回 false：监听/端口文件失败（桌宠降级为无动画，不影响输入法主功能）。
  bool Start();

  // 关闭监听、断开所有桌宠连接、终止线程。
  void Stop();

  // 按键事件（服务进程 ProcessKeyEvent 调用；vk 为 Windows 虚拟键码，
  // make=true 按下 / false 抬起）。best-effort：无桌宠连接时静默丢弃。
  void SendKeyEvent(std::uint32_t vk, std::uint32_t scan, bool make);

  // 输入法激活/停用通知（TSF OnActivated → 桌宠显示/淡化隐藏）。同 best-effort。
  void SendPetVisible(bool visible);

  // 桌宠主动退出（右键"退出桌宠"→ bye 帧）回调：server 据此记住"用户主动退出"，
  // 本次服务生命周期内不再自动拉起（下次切换输入法也不弹，重启服务恢复）。
  void SetOnPetBye(std::function<void()> cb) { on_pet_bye_ = std::move(cb); }

 private:
  static unsigned long __stdcall AcceptLoop(void* param);
  static unsigned long __stdcall ClientThread(void* param);
  void Broadcast(const std::vector<std::uint8_t>& frame);
  void RemoveClient(std::uintptr_t s);

  std::uintptr_t listen_sock_ = 0;     // SOCKET（INVALID_SOCKET 用 0 表示未建）
  std::uintptr_t accept_thread_ = 0;   // HANDLE
  std::atomic<bool> running_{false};
  std::mutex mutex_;
  std::vector<std::uintptr_t> clients_;  // 已通过鉴权的桌宠连接（SOCKET 值）
  std::string token_;                    // 随机 token（防本地进程伪造按键事件）
  std::function<void()> on_pet_bye_;     // 桌宠 bye 回调（用户主动退出标记）
};

}  // namespace ime::server
