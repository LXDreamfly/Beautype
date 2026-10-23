// 皮肤/主题抽象（M1 定稿，PNG 皮肤的扩展点）。
//
// 不变式：候选窗（ui/）只消费 Theme，完全不感知皮肤来源——
// 来源可以是内置默认主题、theme.yaml 配置，或未来的 PNG importer。
// 将来加入 PNG 皮肤 = 新增一个 SkinImporter 实现（+ThemeCompiler 产出
// 本 schema 的 yaml），不修改 UI 与引擎。见 docs/skin-format.md。
#pragma once

#include <cstdint>
#include <string>

namespace ime::skin {

// 颜色统一 0xAARRGGBB。
struct ThemeColors {
  std::uint32_t background = 0xFFF5F5F0;
  std::uint32_t text = 0xFF333333;
  std::uint32_t highlight_text = 0xFF111111;
  std::uint32_t highlight_background = 0xFFE6E6DE;
};

struct ThemeFont {
  std::string family = "Microsoft YaHei";
  int size = 40;  // 逻辑像素。v5.x：固定大字号（不做 DPI 缩放），微软候选窗
                  // 物理字号同量级；仅默认值，用户 theme.yaml 配置优先
};

struct ThemeLayout {
  int padding_x = 12;  // v3.1 边距加大：8→12
  int padding_y = 10;  // v3.1 边距加大：6→10
  int item_spacing = 4;  // v3.1 候选间距加大：2→4
  int item_height = 54;  // v5.x 条目高调大：36→54（微软候选条物理高 ~54px；默认值，yaml 优先）
  int max_items = 8;     // 一页最多显示的候选数
};

// --- v3 皮肤（九宫格整图 + 角色图；docs/skin-format.md §4） ---
enum class SkinMode {
  None,   // 纯色/背景图（v1.2 行为）
  Role,   // 角色图：普通框右侧固定站角色
  Frame,  // 输入框图：整图即候选窗造型（9-patch 拉伸）
};

// 九宫格切线（像素）。默认值与皮肤规范图一致：左右 150 不变形、中间 300 拉伸。
struct SkinBorders {
  int left = 150;
  int right = 150;
  int top = 40;
  int bottom = 40;
};

// 文字排版区（相对整图左上角）。width/height <= 0 = 未指定，
// 渲染时按 frame_image 尺寸推导为整个中间区（x=left, y=top）。
struct SkinTextRegion {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct ThemeSkin {
  SkinMode mode = SkinMode::None;
  std::string role_image;    // 角色图：256×256 PNG 透明底（相对 theme.yaml 目录）
  int role_height = 160;      // 角色图高度（px），用户可拖拽调整
  std::string frame_image;   // 输入框图：600×200 PNG（相对 theme.yaml 目录）
  SkinBorders borders;
  SkinTextRegion text_region;
};

// 主题 = 数据。位图背景等字段为 PNG 皮肤预留（一期不实现，保持默认值）。
struct Theme {
  std::string name = "default";
  ThemeColors colors;
  ThemeFont font;
  ThemeLayout layout;

  // --- PNG 皮肤扩展点（一期不实现，字段先定稿，见 docs/skin-format.md） ---
  // 背景位图资源路径（九宫格切图 / 整图），为空 = 纯色背景
  std::string background_image;   // e.g. "skin://bg.png"（未来 importer 填充）
  std::string border_image;       // 边框九宫格
  // 候选窗整体圆角半径（同时用于高亮块圆角）。默认 16：候选窗四角真圆角，
  // 分层窗口四角 alpha=0 透明（真机根因 2026-10-04：GDI 画 32bpp DIB 不写
  // alpha 导致视觉不透明区域 alpha=0 → 鼠标穿透下层；后处理强制圆角矩形内
  // alpha=0xFF 根治）。0 = 直角。
  int highlight_radius = 16;

  // --- v3 皮肤块（mode=None 时全部忽略 = v1.2 行为；向后兼容） ---
  ThemeSkin skin;
};

// 内置默认主题（无任何位图资源，纯色 + 字体）
Theme DefaultTheme();

// 从 theme.yaml 加载主题（yaml-cpp 解析；schema 见 docs/skin-format.md）。
// 返回 false = 文件不存在或解析失败（调用方可回退 DefaultTheme()）。
bool LoadThemeFromYaml(const std::string& path, Theme* out);

// 把主题完整序列化写入 theme.yaml（设置面板「保存并应用」与首次落盘共用）。
// 背景图/圆角等 PNG 扩展字段一并写入；颜色带引号（# 开头避免被当注释）。
bool WriteThemeToYaml(const std::string& path, const Theme& theme);

// 解析 "#RRGGBB" / "#AARRGGBB" → 0xAARRGGBB；失败返回默认 0xFF000000。
std::uint32_t ParseHexColor(const std::string& s);

// 0xAARRGGBB → 8 位十六进制字符串（大写字面，含 AA，如 "FFF5F5F0"）。
std::string HexColor(std::uint32_t argb);

}  // namespace ime::skin
