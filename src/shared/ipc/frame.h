// 协议帧编解码：4 字节大端长度前缀 + UTF-8 JSON 载荷。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rime_beautify::ipc {

// 单帧载荷上限（防恶意/异常输入耗尽内存）。
inline constexpr std::uint32_t kMaxFrameBytes = 1u << 20;  // 1 MiB

struct Frame {
  std::uint32_t length = 0;            // 载荷字节数
  std::vector<std::uint8_t> payload;   // 载荷（UTF-8 JSON）
};

// 编码：长度前缀（大端）+ 载荷。
std::vector<std::uint8_t> EncodeFrame(std::string_view json);

enum class DecodeStatus {
  kNeedMore,  // 缓冲区数据不足，等待更多字节
  kInvalid,   // 长度前缀非法（超过 kMaxFrameBytes）
  kOk,        // 成功解码一帧
};

// 从累积缓冲区解码一帧。
// kOk 时 *out 为帧内容；*consumed_out 为本次已消费字节数（kOk / kInvalid 时有效）。
DecodeStatus TryDecodeFrame(std::string_view buffer, std::size_t* consumed_out, Frame* out);

}  // namespace rime_beautify::ipc
