// CI 冒烟：皮肤管线（Theme/theme.yaml/Importer/Applier）+ 候选窗（测量/离屏渲染）。
// 验证「候选窗只消费 Theme、皮肤来源可插拔」的 M1 架构不变式。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "skin/theme.h"
#include "skin/theme_compiler.h"
#include "skin/skin_importer.h"
#include "skin/skin_applier.h"
#include "ui/candidate_window.h"

namespace {

bool Expect(bool cond, const char* what, int* failures) {
  if (!cond) {
    std::printf("[test-ui] FAIL: %s\n", what);
    ++(*failures);
    return false;
  }
  return true;
}

}  // namespace

int main() {
  std::printf("[test-ui] === M1 皮肤管线 + 候选窗冒烟 ===\n");
  int failures = 0;

  // 1) 内置默认主题
  const ime::skin::Theme theme = ime::skin::DefaultTheme();
  Expect(theme.name == "default", "默认主题名", &failures);
  Expect(theme.colors.background == 0xFFF5F5F0, "默认背景色", &failures);

  // 2) theme.yaml 解析（schema 见 docs/skin-format.md）
  const auto yaml_path = std::filesystem::temp_directory_path() /
                         "rime_beautify_ui_smoke_theme.yaml";
  {
    std::ofstream out(yaml_path, std::ios::trunc);
    out << "name: test\n"
           "colors:\n"
           "  background: \"#123456\"\n"
           "  text: \"#ABCDEF\"\n"
           "  highlight_background: \"#11223344\"\n"
           "font:\n"
           "  size: 18\n"
           "layout:\n"
           "  item_height: 24\n"
           "  max_items: 6\n";
    out.close();
  }
  ime::skin::Theme t2;
  if (Expect(ime::skin::LoadThemeFromYaml(yaml_path.string(), &t2), "theme.yaml 解析", &failures)) {
    Expect(t2.name == "test", "yaml 主题名", &failures);
    Expect(t2.colors.background == 0xFF123456, "yaml 背景色 #RRGGBB", &failures);
    Expect(t2.colors.highlight_background == 0x11223344, "yaml 高亮背景 #AARRGGBB", &failures);
    Expect(t2.font.size == 18, "yaml 字号", &failures);
    Expect(t2.layout.item_height == 24 && t2.layout.max_items == 6, "yaml 布局", &failures);
  }
  std::error_code ec;
  std::filesystem::remove(yaml_path, ec);

  // 3) 皮肤导入注册表：默认 importer 命中
  ime::skin::Theme t3;
  Expect(ime::skin::ImportSkin("default", &t3) && t3.name == "default",
         "ImportSkin(default)", &failures);

  // 4) SkinApplier：主题落盘 theme.yaml
  const auto cfg_dir = std::filesystem::temp_directory_path() / "rime_beautify_ui_smoke_cfg";
  Expect(ime::skin::ApplyThemeToConfig(t3, cfg_dir.string()), "ApplyThemeToConfig", &failures);
  std::filesystem::remove_all(cfg_dir, ec);

  // 5) 候选窗测量：随候选数单调增长（横排布局：候选数增长 → 窗口变宽）
  ime::ui::CandidateWindow win;
  win.SetTheme(theme);
  const auto one = win.Measure(theme, {"你"});
  const auto two = win.Measure(theme, {"你", "好"});
  Expect(one.cx > 0 && one.cy > 0, "单候选尺寸有效", &failures);
  Expect(two.cx > one.cx, "候选数增长 → 窗口变宽（横排）", &failures);

  // 6) 离屏渲染：候选文本真实绘制（存在非背景像素）
  std::vector<std::uint8_t> argb(static_cast<std::size_t>(two.cx) * two.cy * 4u);
  const bool drawn = win.RenderToMemoryDib({"你好", "您好"}, 0, two.cx, two.cy, argb.data());
  Expect(drawn, "离屏渲染存在文本像素", &failures);

  // 7) WriteThemeToYaml → LoadThemeFromYaml 往返（设置面板「保存并应用」链路）：
  //    颜色/字体/布局/PNG 扩展字段（背景图绝对路径、圆角）必须完整还原。
  const auto round_path = std::filesystem::temp_directory_path() /
                          "rime_beautify_ui_smoke_roundtrip.yaml";
  {
    ime::skin::Theme t7 = ime::skin::DefaultTheme();
    t7.colors.background = 0xFF123456;
    t7.colors.highlight_background = 0x11223344;
    t7.font.family = "SimHei";
    t7.font.size = 20;
    t7.layout.item_height = 26;
    t7.layout.max_items = 5;
    t7.background_image =
        (std::filesystem::temp_directory_path() / "bg_roundtrip.png").string();
    t7.highlight_radius = 8;
    if (Expect(ime::skin::WriteThemeToYaml(round_path.string(), t7), "WriteThemeToYaml",
               &failures)) {
      ime::skin::Theme t8;
      if (Expect(ime::skin::LoadThemeFromYaml(round_path.string(), &t8), "往返回读",
                 &failures)) {
        Expect(t8.colors.background == 0xFF123456 &&
                   t8.colors.highlight_background == 0x11223344,
               "往返颜色", &failures);
        Expect(t8.font.family == "SimHei" && t8.font.size == 20, "往返字体", &failures);
        Expect(t8.layout.item_height == 26 && t8.layout.max_items == 5, "往返布局",
               &failures);
        Expect(t8.highlight_radius == 8, "往返圆角", &failures);
        // 背景图绝对路径往返不变；再验证相对路径按 yaml 目录解析
        Expect(t8.background_image == t7.background_image, "往返背景图(绝对路径)",
               &failures);
        const auto rel_path = std::filesystem::temp_directory_path() /
                              "rime_beautify_ui_smoke_rel.yaml";
        {
          std::ofstream out(rel_path, std::ios::trunc);
          out << "background_image: \"bg.png\"\n";
          out.close();
        }
        ime::skin::Theme t9;
        if (Expect(ime::skin::LoadThemeFromYaml(rel_path.string(), &t9), "相对背景图解析",
                   &failures)) {
          Expect(!t9.background_image.empty() && t9.background_image.find("bg.png") !=
                                                      std::string::npos,
                 "相对背景图 → 配置目录绝对路径", &failures);
        }
        std::filesystem::remove(rel_path, ec);
      }
    }
  }
  std::filesystem::remove(round_path, ec);

  // 8) v3 皮肤块解析：mode/role_image/frame_image/borders/text_region（docs/skin-format.md §4）
  const auto skin_yaml = std::filesystem::temp_directory_path() /
                         "rime_beautify_ui_smoke_skin.yaml";
  {
    std::ofstream out(skin_yaml, std::ios::trunc);
    out << "skin:\n"
           "  mode: frame\n"
           "  role_image: \"cat.png\"\n"
           "  frame_image: \"frame.png\"\n"
           "  borders:\n"
           "    left: 120\n"
           "    right: 130\n"
           "    top: 30\n"
           "    bottom: 35\n"
           "  text_region:\n"
           "    x: 10\n"
           "    y: 20\n"
           "    width: 300\n"
           "    height: 80\n";
    out.close();
  }
  ime::skin::Theme ts;
  if (Expect(ime::skin::LoadThemeFromYaml(skin_yaml.string(), &ts), "v3 skin 块解析",
             &failures)) {
    Expect(ts.skin.mode == ime::skin::SkinMode::Frame, "v3 mode=frame", &failures);
    Expect(ts.skin.role_image.find("cat.png") != std::string::npos,
           "v3 role_image 相对路径", &failures);
    Expect(ts.skin.frame_image.find("frame.png") != std::string::npos,
           "v3 frame_image 相对路径", &failures);
    Expect(ts.skin.borders.left == 120 && ts.skin.borders.right == 130 &&
               ts.skin.borders.top == 30 && ts.skin.borders.bottom == 35,
           "v3 borders", &failures);
    Expect(ts.skin.text_region.x == 10 && ts.skin.text_region.y == 20 &&
               ts.skin.text_region.width == 300 && ts.skin.text_region.height == 80,
           "v3 text_region", &failures);
  }
  std::filesystem::remove(skin_yaml, ec);

  // 9) v3 皮肤块 yaml 往返（设置面板「保存并应用」链路必须完整还原 skin 字段）
  const auto skin_round = std::filesystem::temp_directory_path() /
                          "rime_beautify_ui_smoke_skin_round.yaml";
  {
    ime::skin::Theme t = ime::skin::DefaultTheme();
    t.skin.mode = ime::skin::SkinMode::Role;
    t.skin.role_image = "cat.png";
    t.skin.frame_image = "frame.png";
    t.skin.borders = {100, 110, 20, 25};
    t.skin.text_region = {5, 6, 200, 50};
    if (Expect(ime::skin::WriteThemeToYaml(skin_round.string(), t), "v3 skin 写入",
               &failures)) {
      ime::skin::Theme u;
      if (Expect(ime::skin::LoadThemeFromYaml(skin_round.string(), &u), "v3 skin 往返回读",
                 &failures)) {
        Expect(u.skin.mode == ime::skin::SkinMode::Role, "v3 往返 mode=role", &failures);
        Expect(u.skin.role_image.find("cat.png") != std::string::npos,
               "v3 往返 role_image", &failures);
        Expect(u.skin.borders.left == 100 && u.skin.borders.right == 110 &&
                   u.skin.borders.top == 20 && u.skin.borders.bottom == 25,
               "v3 往返 borders", &failures);
        Expect(u.skin.text_region.x == 5 && u.skin.text_region.y == 6 &&
                   u.skin.text_region.width == 200 && u.skin.text_region.height == 50,
               "v3 往返 text_region", &failures);
      }
    }
  }
  std::filesystem::remove(skin_round, ec);

  // 10) 九宫格分段纯函数：9 段正尺寸 / 四角与上下左右边不变形 / 中间双向拉伸
  {
    ime::skin::SkinBorders b{150, 150, 40, 40};
    ime::ui::NinePatchRect segs[ime::ui::kNinePatchCount];
    ime::ui::ComputeNinePatch(600, 200, 800, 260, b, segs);
    bool size_ok = true;
    for (int k = 0; k < ime::ui::kNinePatchCount; ++k) {
      if (segs[k].sw <= 0 || segs[k].sh <= 0 || segs[k].dw <= 0 || segs[k].dh <= 0) {
        size_ok = false;
      }
    }
    Expect(size_ok, "九宫格 9 段均为正尺寸", &failures);
    bool corners_ok = true;
    for (int c : {0, 2, 6, 8}) {
      if (segs[c].dw != segs[c].sw || segs[c].dh != segs[c].sh) {
        corners_ok = false;
      }
    }
    Expect(corners_ok, "九宫格四角不变形", &failures);
    Expect(segs[1].dh == segs[1].sh && segs[7].dh == segs[7].sh,
           "九宫格上下边仅水平拉伸", &failures);
    Expect(segs[3].dw == segs[3].sw && segs[5].dw == segs[5].sw,
           "九宫格左右边仅垂直拉伸", &failures);
    Expect(segs[4].dw == 500 && segs[4].dh == 180, "九宫格中间双向拉伸(800-300=500, 260-80=180)",
           &failures);
    // 窗口小于切线时夹紧不越界（单候选极窄场景）
    ime::ui::NinePatchRect tight[ime::ui::kNinePatchCount];
    ime::ui::ComputeNinePatch(600, 200, 120, 40, b, tight);
    bool tight_ok = true;
    for (int k = 0; k < ime::ui::kNinePatchCount; ++k) {
      if (tight[k].dw < 1 || tight[k].dh < 1 || tight[k].dx + tight[k].dw > 120 ||
          tight[k].dy + tight[k].dh > 40) {
        tight_ok = false;
      }
    }
    Expect(tight_ok, "九宫格窗口小于切线时夹紧不越界", &failures);
  }

  // 11) 目录兜底（真机根因 2026-10-03）：目标目录不存在时 WriteThemeToYaml
  //     必须自动创建父目录并成功写入（设置面板「保存并应用」在清理旧配置后不再报错）。
  {
    const auto missing_root = std::filesystem::temp_directory_path() /
                              "rime_beautify_ui_smoke_missing_dir";
    const auto deep_yaml = missing_root / "nested" / "cfg" / "theme.yaml";
    ime::skin::Theme t11 = ime::skin::DefaultTheme();
    t11.name = "dir_fallback";
    if (Expect(ime::skin::WriteThemeToYaml(deep_yaml.string(), t11),
               "目录缺失时 WriteThemeToYaml 自动创建", &failures)) {
      Expect(std::filesystem::is_regular_file(deep_yaml, ec), "自动创建的 yaml 存在",
             &failures);
      ime::skin::Theme u11;
      if (Expect(ime::skin::LoadThemeFromYaml(deep_yaml.string(), &u11), "目录兜底往返回读",
                 &failures)) {
        Expect(u11.name == "dir_fallback", "目录兜底主题名", &failures);
      }
    }
    std::filesystem::remove_all(missing_root, ec);
  }

  if (failures == 0) {
    std::printf("[test-ui] 结论：Theme 抽象 / yaml 解析 / importer 注册表 / applier / "
                "候选窗渲染 / v3 皮肤块+九宫格 全链路 OK\n");
    return 0;
  }
  std::printf("[test-ui] 结果：%d FAILURE(S)\n", failures);
  return 1;
}
