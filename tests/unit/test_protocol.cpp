// 协议常量与端口文件序列化测试：能推翻“版本漂移 / 端口文件解析错位 / 非法输入被接受”等错误机制。
#include "ipc/protocol.h"

#include <cstdio>
#include <string>

using namespace rime_beautify::ipc;

static int g_failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    const bool _ok = !!(cond);                                    \
    if (!_ok) {                                                   \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                              \
    }                                                            \
  } while (0)

int main() {
  // 协议基线
  CHECK(kProtocolVersion == 1);
  CHECK(std::string(kPortFileName) == "ime.port.json");
  CHECK(static_cast<int>(MessageType::kHello) == 1);
  CHECK(static_cast<int>(MessageType::kKeyEvent) == 3);
  CHECK(static_cast<int>(MessageType::kBye) == 6);

  // 端口文件回环
  {
    PortFile pf{1, 43871, "deadbeef0123"};
    const std::string json = PortFileToJson(pf);
    PortFile back;
    CHECK(PortFileFromJson(json, &back));
    CHECK(back.protocol_version == 1);
    CHECK(back.port == 43871);
    CHECK(back.token == "deadbeef0123");
  }
  // 端口文件带空白容忍
  {
    PortFile back;
    CHECK(PortFileFromJson(" { \"port\" : 12345 , \"token\" : \"abc\" , \"protocol_version\" : 1 } ", &back));
    CHECK(back.port == 12345);
    CHECK(back.token == "abc");
    CHECK(back.protocol_version == 1);
  }
  // 非法输入拒绝
  {
    PortFile dummy;
    CHECK(!PortFileFromJson("", &dummy));
    CHECK(!PortFileFromJson("garbage", &dummy));
    CHECK(!PortFileFromJson("{\"protocol_version\":1}", &dummy));       // 缺字段
    CHECK(!PortFileFromJson("{\"protocol_version\":1,\"port\":1,\"token\":\"a\",\"extra\":1}", &dummy));  // 多余字段
    CHECK(!PortFileFromJson("{\"protocol_version\":x,\"port\":1,\"token\":\"a\"}", &dummy));  // 非法数字
  }

  if (g_failures == 0) {
    std::printf("test_protocol: ALL PASS\n");
    return 0;
  }
  std::printf("test_protocol: %d FAILURE(S)\n", g_failures);
  return 1;
}
