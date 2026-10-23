// IPC 协议 v1 常量与类型定义。
// 完整规范见 docs/ipc-protocol.md；本文件为协议的可执行参照实现的一部分。
#pragma once

#include <cstdint>
#include <string>

namespace rime_beautify::ipc {

// 协议版本：任何不兼容变更（帧格式/消息结构/消息类型）必须递增。
inline constexpr int kProtocolVersion = 1;

// IME 把监听端口 + token 写入该文件（位于 runtime 目录），桌宠据此连接。
inline constexpr char kPortFileName[] = "ime.port.json";

// 消息类型。
// 16 起为预留类型（二期实现，一期不发送）：
//   kCommitText 上屏文本  kMouseEvent 鼠标事件  kConfigSync 配置同步
enum class MessageType : std::uint8_t {
  kHello = 1,     // 桌宠→IME：连接鉴权（携带 token）
  kHelloAck = 2,  // IME→桌宠：接受连接（携带协议版本与组件状态）
  kKeyEvent = 3,  // IME→桌宠：按键事件
  kPing = 4,      // 保活
  kPong = 5,      // 保活应答
  kBye = 6,       // 任一方向：正常关闭

  kCommitText = 16,
  kMouseEvent = 17,
  kConfigSync = 18,
};

// 按键事件载荷。
struct KeyEvent {
  std::uint32_t vk = 0;    // 虚拟键码（Windows VK_*）
  std::uint32_t scan = 0;  // 扫描码
  bool make = true;        // true=按下，false=抬起
  std::uint64_t ts = 0;    // 毫秒时间戳
};

// 端口文件内容：IME 写入，桌宠读取。
struct PortFile {
  int protocol_version = kProtocolVersion;
  std::uint16_t port = 0;  // IME 监听端口
  std::string token;       // 连接鉴权 token（随机 hex，防本地进程伪造）
};

// 极简 JSON 序列化（仅本项目固定字段，避免在共享层引入第三方 JSON 依赖）。
std::string PortFileToJson(const PortFile& pf);
// 仅接受本项目生成的固定结构；非法输入返回 false。
bool PortFileFromJson(const std::string& json, PortFile* pf);

}  // namespace rime_beautify::ipc
