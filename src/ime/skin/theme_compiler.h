// 主题编译器接口（M1 定稿，PNG 一期不实现）。
//
// 职责：把「PNG 切图 + 参数清单」编译为 theme.yaml（与手写 yaml 同 schema，
// 见 docs/skin-format.md），使 PNG 皮肤与手写主题在 UI 侧完全等价。
// 一期仅定义契约与产出格式，PngThemeCompiler 实现留待加 PNG 皮肤时落地；
// 届时只需新增该实现并注册，不修改 UI/引擎/Theme。
#pragma once

#include <string>

namespace ime::skin {

class ThemeCompiler {
 public:
  virtual ~ThemeCompiler() = default;

  // 把 png_skin_dir（切图 + skin.yaml 参数清单）编译为 out_yaml_path（theme.yaml）。
  // 返回 false = 资源缺失 / 参数非法（调用方回退 DefaultTheme）。
  virtual bool Compile(const std::string& png_skin_dir, const std::string& out_yaml_path) = 0;
};

// 一期实现：PNG 皮肤包 = skin.yaml（与 theme.yaml 同 schema）+ 切图 → theme.yaml。
// 资源（mode 引用的角色/输入框图）缺失时不产出，保证 theme.yaml 永远指向存在的图。
class PngThemeCompiler final : public ThemeCompiler {
 public:
  bool Compile(const std::string& png_skin_dir, const std::string& out_yaml_path) override;
};

}  // namespace ime::skin
