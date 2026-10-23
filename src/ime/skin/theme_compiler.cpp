// 皮肤编译器一期实现：PNG 皮肤包（skin.yaml + 切图）→ theme.yaml。
// skin.yaml 与 theme.yaml 同 schema（docs/skin-format.md §4），相对图片路径
// 以 skin.yaml 所在目录为基准解析；编译 = 资源存在性校验 + schema 转发。
#include "theme_compiler.h"

#include <filesystem>

#include "theme.h"

namespace ime::skin {

bool PngThemeCompiler::Compile(const std::string& png_skin_dir,
                               const std::string& out_yaml_path) {
  const auto params = std::filesystem::path(png_skin_dir) / "skin.yaml";
  if (!std::filesystem::exists(params)) {
    return false;
  }
  Theme theme;
  if (!LoadThemeFromYaml(params.string(), &theme)) {
    return false;
  }
  // 资源存在性校验：mode 引用的图片必须存在（避免写出即坏的 theme.yaml）
  if (theme.skin.mode == SkinMode::Role && !theme.skin.role_image.empty()) {
    if (!std::filesystem::exists(theme.skin.role_image)) {
      return false;
    }
  }
  if (theme.skin.mode == SkinMode::Frame && !theme.skin.frame_image.empty()) {
    if (!std::filesystem::exists(theme.skin.frame_image)) {
      return false;
    }
  }
  if (theme.skin.mode == SkinMode::None) {
    // 纯色/背景图不构成「PNG 皮肤包」，拒绝编译
    return false;
  }
  return WriteThemeToYaml(out_yaml_path, theme);
}

}  // namespace ime::skin
