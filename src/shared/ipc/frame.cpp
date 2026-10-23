#include "ipc/frame.h"

namespace rime_beautify::ipc {

std::vector<std::uint8_t> EncodeFrame(std::string_view json) {
  const std::uint32_t len = static_cast<std::uint32_t>(json.size());
  std::vector<std::uint8_t> out;
  out.reserve(4 + json.size());
  out.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
  out.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
  out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
  out.push_back(static_cast<std::uint8_t>(len & 0xFF));
  out.insert(out.end(), json.begin(), json.end());
  return out;
}

DecodeStatus TryDecodeFrame(std::string_view buffer, std::size_t* consumed_out, Frame* out) {
  if (buffer.size() < 4) {
    return DecodeStatus::kNeedMore;
  }
  const std::uint32_t len =
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buffer[0])) << 24) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buffer[1])) << 16) |
      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(buffer[2])) << 8) |
      static_cast<std::uint32_t>(static_cast<std::uint8_t>(buffer[3]));
  if (len > kMaxFrameBytes) {
    if (consumed_out) {
      *consumed_out = buffer.size();
    }
    return DecodeStatus::kInvalid;
  }
  if (buffer.size() < 4u + len) {
    return DecodeStatus::kNeedMore;
  }
  Frame f;
  f.length = len;
  f.payload.assign(buffer.begin() + 4, buffer.begin() + 4 + len);
  if (consumed_out) {
    *consumed_out = 4u + len;
  }
  if (out) {
    *out = std::move(f);
  }
  return DecodeStatus::kOk;
}

}  // namespace rime_beautify::ipc
