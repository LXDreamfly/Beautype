#include "skin_applier.h"

#include <cstdio>
#include <filesystem>
#include "log/log.h"

// 统一日志文件（rime_server.exe 内为 rime_server.log，rime_tsf.dll 内为
// rime_tsf.log；由各二进制内的 ime::g_log 定义提供；正式版无 stdout）
namespace ime { extern FILE* g_log; }

namespace ime::skin {

// 落盘 theme.yaml：与 LoadThemeFromYaml 读取的文件名一致（历史上曾写
// beautify.yaml 而加载读 theme.yaml，用户改配置不生效——已统一）。
// 序列化逻辑收敛在 theme.cpp 的 WriteThemeToYaml（设置面板「保存并应用」
// 与首次落盘共用同一份实现，保证手改/面板改格式完全一致）。
bool ApplyThemeToConfig(const Theme& theme, const std::string& config_dir) {
  std::error_code ec;
  std::filesystem::create_directories(config_dir, ec);
  if (ec) {
    ::ime::LogF( "[skin] 无法创建配置目录 %s: %s\n", config_dir.c_str(), ec.message().c_str());
    return false;
  }
  // theme.yaml 存 config 根目录（图片在 skins\ 子目录，frame_image 存相对路径）
  const auto path = std::filesystem::path(config_dir) / "theme.yaml";
  if (!WriteThemeToYaml(path.string(), theme)) {
    return false;
  }
  ::ime::LogF( "[skin] 主题已写入 %s (name=%s)\n", path.string().c_str(),
              theme.name.c_str());
  return true;
}

}  // namespace ime::skin
