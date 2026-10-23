// 皮肤应用接口（M1 定稿）：把主题落地为配置并触发 IME 重载。
// 一期实现：把完整主题写入 IME 配置目录下的 theme.yaml（与加载同名），
// 供下次启动按文件加载；重载通过 service->SetTheme 热更新。
// PNG 皮肤加入时此接口不变。
#pragma once

#include <string>

#include "theme.h"

namespace ime::skin {

class SkinApplier {
 public:
  virtual ~SkinApplier() = default;

  // 应用主题：把主题来源（名称/路径）写入 config_dir 下的 beautify.yaml，
  // 返回写入是否成功。theme 本身已由调用方通过 ImportSkin 得到。
  virtual bool Apply(const Theme& theme, const std::string& config_dir) = 0;
};

// 默认实现：在 config_dir 写入 theme.yaml（与 LoadThemeFromYaml 同名），
// 内容为完整默认主题 + 注释模板（用户改字体/颜色/背景图后重启生效）。
// 序列化实现在 theme.cpp 的 WriteThemeToYaml（设置面板与首次落盘共用）。
// 未来 PngImporter/ThemeCompiler 落地后，名称可为 skin.yaml 相对路径。
bool ApplyThemeToConfig(const Theme& theme, const std::string& config_dir);

}  // namespace ime::skin
