// 皮肤导入接口（M1 定稿）：皮肤来源可插拔。
//
// 一期注册：DefaultImporter（内置默认主题，导入空路径即得 DefaultTheme()）。
// 二期/后续：PngImporter —— 把 PNG 皮肤包（切图 + 参数清单）导入为 skin::Theme
// （位图资源 + 配色/布局），再经 ThemeCompiler 产出 theme.yaml（与手写 yaml 同
// schema）。候选窗只消费 Theme，因此新增 importer 不影响 UI 与引擎。
// 详细设计见 docs/skin-format.md。
#pragma once

#include <string>
#include <vector>

#include "theme.h"

namespace ime::skin {

class SkinImporter {
 public:
  virtual ~SkinImporter() = default;

  // 该 importer 是否能处理此路径（文件/目录/内置名）。
  virtual bool CanImport(const std::string& path) const = 0;
  // 导入并填充 Theme；失败返回 false（调用方回退 DefaultTheme）。
  virtual bool Import(const std::string& path, Theme* out) const = 0;
};

// 注册表：按注册顺序尝试，直到某个 importer 成功。
void RegisterSkinImporter(const SkinImporter* importer);
// 导入皮肤；无 importer 命中时返回 false。
bool ImportSkin(const std::string& path, Theme* out);

// 内置默认主题 importer（path 为空串或 "default" 时命中）。
const SkinImporter* DefaultImporter();

}  // namespace ime::skin
