// 输入法前端骨架桩（仅 RIME_BEAUTIFY_BUILD_IME=OFF 时编译，本地无 librime 场景）。
// 完整前端入口见 main.cpp。
#include <cstdio>
#include <cstdlib>
#include <filesystem>

int main() {
  std::printf("[rime-ime] skeleton v0 (librime frontend, TSF/UI/skin/push coming in M1)\n");
  const char* local = std::getenv("LOCALAPPDATA");
  if (local != nullptr) {
    const auto runtime = std::filesystem::path(local) / "rime_beautify" / "runtime";
    std::error_code ec;
    std::filesystem::create_directories(runtime, ec);
    if (!ec) {
      std::printf("[rime-ime] runtime dir ready: %s\n", runtime.string().c_str());
    }
  }
  return 0;
}
