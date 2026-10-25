// 帧编解码测试：能推翻“帧解析损坏 / 上限失效 / 一包多帧错位”等错误机制。
#include "ipc/frame.h"

#include <cstdio>
#include <string>

using namespace rime_beautify::ipc;

static int g_failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                              \
    }                                                            \
  } while (0)

static std::string_view AsView(const std::vector<std::uint8_t>& bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

int main() {
  // 1. 编解码回环
  {
    const std::string json = "{\"v\":1,\"type\":\"key_event\"}";
    const auto bytes = EncodeFrame(json);
    Frame f;
    std::size_t consumed = 0;
    const DecodeStatus st = TryDecodeFrame(AsView(bytes), &consumed, &f);
    CHECK(st == DecodeStatus::kOk);
    CHECK(consumed == bytes.size());
    CHECK(f.payload.size() == json.size());
    CHECK(std::string(f.payload.begin(), f.payload.end()) == json);
  }
  // 2. 空载荷帧
  {
    const auto bytes = EncodeFrame("");
    Frame f;
    std::size_t consumed = 0;
    CHECK(TryDecodeFrame(AsView(bytes), &consumed, &f) == DecodeStatus::kOk);
    CHECK(consumed == 4u);
    CHECK(f.payload.empty());
  }
  // 3. 半包 → kNeedMore
  {
    const auto bytes = EncodeFrame("{}");
    Frame f;
    std::size_t consumed = 0;
    CHECK(TryDecodeFrame(AsView(bytes).substr(0, bytes.size() - 1), &consumed, &f) ==
          DecodeStatus::kNeedMore);
  }
  // 4. 空缓冲区 / 不足 4 字节 → kNeedMore
  {
    Frame f;
    std::size_t consumed = 0;
    CHECK(TryDecodeFrame(std::string_view(), &consumed, &f) == DecodeStatus::kNeedMore);
    CHECK(TryDecodeFrame(std::string_view("ab", 2), &consumed, &f) == DecodeStatus::kNeedMore);
  }
  // 5. 长度前缀超过上限 → kInvalid
  {
    const char hdr[4] = {static_cast<char>(0xFF), static_cast<char>(0xFF),
                         static_cast<char>(0xFF), static_cast<char>(0xFF)};
    Frame f;
    std::size_t consumed = 0;
    CHECK(TryDecodeFrame(std::string_view(hdr, 4), &consumed, &f) == DecodeStatus::kInvalid);
  }
  // 6. 一包多帧（连续解码）
  {
    const auto b1 = EncodeFrame("{\"a\":1}");
    const auto b2 = EncodeFrame("{\"b\":2}");
    std::string buf;
    buf.append(reinterpret_cast<const char*>(b1.data()), b1.size());
    buf.append(reinterpret_cast<const char*>(b2.data()), b2.size());
    Frame f;
    std::size_t consumed = 0;
    CHECK(TryDecodeFrame(buf, &consumed, &f) == DecodeStatus::kOk);
    CHECK(consumed == b1.size());
    const std::string_view rest(buf.data() + consumed, buf.size() - consumed);
    CHECK(TryDecodeFrame(rest, &consumed, &f) == DecodeStatus::kOk);
    CHECK(consumed == b2.size());
  }

  if (g_failures == 0) {
    std::printf("test_frame: ALL PASS\n");
    return 0;
  }
  std::printf("test_frame: %d FAILURE(S)\n", g_failures);
  return 1;
}
