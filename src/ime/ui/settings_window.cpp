// 可视化皮肤设置面板 v2（自绘圆角卡片风格，「敲可爱」粉色系）。
// 布局：圆角卡片分区（颜色/字体/背景图/布局）+ 自绘现代按钮 + 大圆角色块，
// 保存后写 theme.yaml 并热更新候选窗。
// 交互：ChooseColor / ChooseFont / GetOpenFileName 通用对话框（官方用法见头文件注释）。
// 实现参考：小狼毫 Weasel 候选窗自绘思路（WS_EX_LAYERED + GDI+）与本仓库
// candidate_window 的 GDI+ 渲染惯例；所有数值均为本文件自有 UI 规范。
// NOMINMAX 必须在任何 Windows 头之前（settings_window.h 内含 windows.h，
// 否则 std::max/std::min 被宏污染 → C2589）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "settings_window.h"

#include "../fs_util.h"
#include "../server/resource.h"  // IDI_BEAUTYPE：设置窗口任务栏图标（蝴蝶 logo）
#include "log/log.h"  // v2.1：保存链路诊断日志



#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

#include <windows.h>
#include <gdiplus.h>

#include "skin/theme.h"
#include "log/log.h"
#include "i18n/strings.h"

// 统一日志文件（rime_server.exe 内为 rime_server.log；正式版无 stdout）
namespace ime { extern FILE* g_log; }

namespace ime::ui {
namespace {

using Gdiplus::Bitmap;
using Gdiplus::Color;
using Gdiplus::DashStyle;
using Gdiplus::DashStyleDash;
using Gdiplus::Font;
using Gdiplus::FontStyle;
using Gdiplus::FontStyleBold;
using Gdiplus::FontStyleRegular;
using Gdiplus::Graphics;
using Gdiplus::GraphicsPath;
using Gdiplus::LinearGradientBrush;
using Gdiplus::PathGradientBrush;
using Gdiplus::Pen;
using Gdiplus::RectF;
using Gdiplus::SmoothingMode;
using Gdiplus::SmoothingModeAntiAlias;
using Gdiplus::SolidBrush;
using Gdiplus::StringFormat;
using Gdiplus::StringAlignmentCenter;
using Gdiplus::TextRenderingHint;
using Gdiplus::TextRenderingHintClearTypeGridFit;
using Gdiplus::UnitPixel;

// ---- Windows 系统风格配色（黑白简洁，跟随系统浅色/深色） ----
// 确认稿 settings_ui_preview_v2.html 的深浅基调 + 主按钮黑白化
// （v2 的蓝色 0078D7 已按要求去掉；深色模式 = 浅色反转）。
struct Palette {
  std::uint32_t bg_top;          // 面板底（上）
  std::uint32_t bg_bottom;       // 面板底（下）
  std::uint32_t card;            // 卡片底
  std::uint32_t card_border;     // 卡片描边
  std::uint32_t text;            // 正文
  std::uint32_t text_sub;        // 副文字
  std::uint32_t accent;          // 标题/分区名（黑白化：同正文色）
  std::uint32_t swatch_border;   // 色块描边
  std::uint32_t preview_border;  // 预览虚线框
  std::uint32_t align_bg;        // 文字区对齐预览底色（设计稿 align-preview）
  std::uint32_t btn_primary;         // 主按钮底（保存并应用）
  std::uint32_t btn_primary_hover;
  std::uint32_t btn_primary_down;
  std::uint32_t btn_primary_text;    // 主按钮文字
  std::uint32_t btn_secondary;       // 次要按钮底（恢复默认/关闭/选择…）
  std::uint32_t btn_secondary_hover;
  std::uint32_t btn_secondary_down;
  std::uint32_t btn_secondary_border;
  std::uint32_t btn_secondary_text;  // 次要按钮文字
  std::uint32_t edit_bg;             // 编辑框底（圆角自绘）
  std::uint32_t edit_border;         // 编辑框描边
};

// 浅色模式（确认稿 v2 基调：F0F0F0 背景/白卡片/深灰文字；主按钮黑底白字）
constexpr Palette kPaletteLight{
    0xFFF0F0F0, 0xFFF0F0F0,              // 面板底：浅灰（无渐变）
    0xFFFFFFFF, 0xFFE0E0E0,              // 卡片：白
    0xFF333333, 0xFF666666,              // 文字
    0xFF333333,                          // 标题/分区名：深灰
    0xFFCCCCCC, 0xFFCCCCCC, 0xFFE8E8E8,  // 色块/预览描边 + 对齐预览底色
    0xFF1A1A1A, 0xFF333333, 0xFF000000,  // 主按钮：黑底（hover 略浅/按下压深）
    0xFFFFFFFF,                          // 主按钮文字：白
    0xFFF0F0F0, 0xFFE8E8E8, 0xFFE0E0E0, 0xFFCCCCCC,  // 次要按钮：浅灰底+灰描边
    0xFF333333,                          // 次要按钮文字：深灰
    0xFFFFFFFF, 0xFFCCCCCC,              // 编辑框：白底浅灰描边（浅色模式）
};

// 深色模式（反转：近黑背景/深灰卡片/白文字；主按钮白底黑字）
constexpr Palette kPaletteDark{
    0xFF202020, 0xFF202020,              // 面板底：近黑
    0xFF2D2D2D, 0xFF2A2A2A,              // 卡片：深灰（v4.4 描边 3C3C3C→2A2A2A：原 60 在深色下像亮白边）
    0xFFFFFFFF, 0xFFCCCCCC,              // 文字
    0xFFFFFFFF,                          // 标题/分区名：白
    0xFF6E6E6E, 0xFF555555, 0xFF111111,  // 色块/预览描边（色块边略亮，深色下黑块可读）+ 对齐预览底色
    // 2026-10-10 深色模式白色太突兀：主按钮/radio 选中态纯白底 → 浅灰 E8E8E8
    //（黑字保持，浅灰底可读；hover 再暗一档）
    0xFFE8E8E8, 0xFFDDDDDD, 0xFFE8E8E8,  // 主按钮：浅灰底（原纯白 FFFFFF 突兀）
    0xFF1A1A1A,                          // 主按钮文字：黑
    0xFF3C3C3C, 0xFF4C4C4C, 0xFF333333, 0xFF2A2A2A,  // 次要按钮：深灰底+暗描边（描边刻意压暗——
                                                    // 原 555555 在深色界面像「白边」，用户 2026-10-10 反馈）
    0xFFFFFFFF,                          // 次要按钮文字：白
    0xFF3C3C3C, 0xFF4A4A4A,              // 编辑框：深灰底稍亮描边（深色模式；
                                        // 原系统默认白底直角方框太突兀，用户 2026-10-10 反馈）
};

// 当前调色板（打开时按系统主题选择；WM_SETTINGCHANGE 实时切换）
static const Palette* g_pal = &kPaletteLight;

// 系统深浅色检测：读注册表 AppsUseLightTheme（0=深色，1=浅色）。
// 官方 weasel 的 color_scheme_dark 深色跟随同条件（Windows 10 1809+）。
bool IsSystemDarkMode() {
  HKEY hk = nullptr;
  if (RegOpenKeyExW(
          HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
          KEY_READ, &hk) != ERROR_SUCCESS) {
    return false;
  }
  DWORD value = 1, size = sizeof(value), type = 0;
  const LONG ret = RegQueryValueExW(hk, L"AppsUseLightTheme", nullptr, &type,
                                    reinterpret_cast<LPBYTE>(&value), &size);
  RegCloseKey(hk);
  return ret == ERROR_SUCCESS && value == 0;
}

void RefreshPalette() {
  g_pal = IsSystemDarkMode() ? &kPaletteDark : &kPaletteLight;
}

// 按钮圆角外白边修复：BUTTON 子控件擦背景用系统默认刷（浅灰/白），自绘只
// 覆盖圆角内，圆角 4 角会透出浅色（用户反馈「所有圆角按钮都有奇怪的白边」）。
// 父窗口 WM_CTLCOLORBTN 返回卡片色刷子 → 圆角外 = 卡片背景，与界面融合；
// 深浅色切换（调色板 card 变化）时重建。
HBRUSH GetCardBrush() {
  static HBRUSH brush = nullptr;
  static std::uint32_t last_card = 0;
  if (brush == nullptr || last_card != g_pal->card) {
    if (brush != nullptr) DeleteObject(brush);
    brush = CreateSolidBrush(RGB((g_pal->card >> 16) & 0xFF,
                                 (g_pal->card >> 8) & 0xFF, g_pal->card & 0xFF));
    last_card = g_pal->card;
  }
  return brush;
}

// 面板底色刷（bg_top）：2026-10-22 按钮圆角"死角"根因修复——按钮子控件
// 的圆角外 4 角由系统用 WM_CTLCOLORBTN 返回的刷子擦背景。此前返回卡片色
// （card），但按钮实际铺在面板底色（bg_top）上：浅色模式白(FFFFFF) vs
// 面板(F0F0F0)、深色模式(2D2D2D) vs 面板(202020)，4 角与周围明显色差 =
// 用户实拍"按钮有多余的死角"。改为返回面板底色后圆角外与面板完全融合。
HBRUSH GetPanelBrush() {
  static HBRUSH brush = nullptr;
  static std::uint32_t last_panel = 0;
  if (brush == nullptr || last_panel != g_pal->bg_top) {
    if (brush != nullptr) DeleteObject(brush);
    brush = CreateSolidBrush(RGB((g_pal->bg_top >> 16) & 0xFF,
                                 (g_pal->bg_top >> 8) & 0xFF,
                                 g_pal->bg_top & 0xFF));
    last_panel = g_pal->bg_top;
  }
  return brush;
}

// v4.4：编辑框/下拉框底色刷（edit_bg）——COMBOBOX 静态文本部分与下拉列表
// 深色化用（真机反馈下拉框白底突兀）。随调色板深浅切换重建。
HBRUSH GetEditBrush() {
  static HBRUSH brush = nullptr;
  static std::uint32_t last_edit = 0;
  if (brush == nullptr || last_edit != g_pal->edit_bg) {
    if (brush != nullptr) DeleteObject(brush);
    brush = CreateSolidBrush(RGB((g_pal->edit_bg >> 16) & 0xFF,
                                 (g_pal->edit_bg >> 8) & 0xFF, g_pal->edit_bg & 0xFF));
    last_edit = g_pal->edit_bg;
  }
  return brush;
}

constexpr int kDlgW = 460;
constexpr int kDlgH = 748;  // v3.7：720→748（皮肤模式卡 196→224 内部行距再拉开）

// ---- DPI 缩放（2026-10-23）：声明 Per-Monitor V2 后系统不再虚拟化，固定像素
// 布局在 >100% 缩放下会变小/错位。引入全局 scale（当前窗口 dpi/96），DS(v) 把
// 布局常量按当前 DPI 放大；DrawTextC/FillRoundRect/DrawRoundRect 内部自动缩放，
// 命中矩形/控件坐标/窗口尺寸用 DS()。RunSettingsDialog/RunColorPicker 创建前设值。
static float g_dpi_scale = 1.0f;
inline int DS(int v) { return static_cast<int>(static_cast<float>(v) * g_dpi_scale + 0.5f); }

// 窗口样式：WS_THICKFRAME 启用「鼠标拖边缘自由调整大小」（正常窗口体验，
// 用户 2026-10-10 反馈「窗口大小不能变化」）。最小尺寸由 WM_GETMINMAXINFO 钳制
// （kDlgW×kDlgH 客户区），放大时布局按宽度自适应重排（RelayoutControls）。
// 注意：不带 WS_VISIBLE——首帧防闪（m1 §30）：创建即显示会露白底空窗 +
// 20 余控件逐个绘制 = 打开第一秒"黑白色块"闪烁；改为控件建完首帧画完再
// ShowWindow（见 RunSettingsDialog 末尾）。
constexpr DWORD kSettingsStyle =
    WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;

// 控件 ID（CreateWindowExW 的 hMenu 参数即子控件 ID）
enum : int {
  kSwatchBg = 100,   // 背景色块
  kSwatchText,       // 正文色块
  kSwatchHlText,     // 高亮文字色块
  kSwatchHlBg,       // 高亮底色块
  kLabelFont,        // 字体摘要
  kLabelBg,          // 背景图摘要
  kBtnFont,
  kBtnBg,
  kBtnBgClear,
  kRadioRole,        // v3：皮肤模式二选一——角色图片
  kRadioFrame,       // v3：皮肤模式二选一——输入框图片
  kBtnRoleImage,     // v3：上传角色图片
  kBtnFrameImage,    // v3：上传输入框图片
  kEditTrX,          // v3：文字区域对齐 X
  kEditTrY,          // v3：文字区域对齐 Y
  kEditTrW,          // v3：文字区域对齐 宽
  kEditTrH,          // v3：文字区域对齐 高
  kComboPreset,      // v3：配置管理——配置下拉
  kBtnPresetLoad,    // v3：配置管理——加载
  kBtnPresetDelete,  // v3：配置管理——删除
  kEditPresetName,   // v3：配置管理——配置名输入
  kBtnPresetSave,    // v3：配置管理——保存当前配置
  kBtnReset,
  kBtnSave,
  kBtnClose,
  kBtnOpenFolder,   // v4.5.3：打开图片保存目录（config 文件夹）
};

struct SettingsContext {
  skin::Theme draft;                  // 编辑草稿（保存前不写盘）
  std::function<void(const skin::Theme&)> on_apply;  // 保存并应用回调（服务端换肤）
  std::string config_dir;
  HWND dlg = nullptr;
  bool applied = false;               // 是否点过「保存并应用」
  std::uint32_t swatch_colors[4] = {};  // 色块当前色（与 draft 同步）
  RECT swatch_hex_rc[4] = {};        // 颜色卡 hex 文字区（客户区，PaintPanel 填充→点击复制）
  int hex_hover = -1;                // 鼠标悬停的 hex 下标（-1 无），供变色提示可复制

  // ---- v3 文字区域对齐（输入框模式）----
  // 对齐预览的视图状态：PaintPanel 每次绘制时更新，供鼠标命中/拖拽换算。
  bool align_has_img = false;   // 输入框模式且 frame_image 可解码
  int align_img_x = 0;          // 输入框图在预览区内显示矩形（左）
  int align_img_y = 0;          // （上）
  int align_img_w = 0;          // （宽）
  int align_img_h = 0;          // （高）
  float align_scale = 1.0f;     // 预览缩放（图片坐标 → 预览坐标）
  RECT align_preview_rc{};      // 预览区矩形（命中测试用，客户区坐标）
  bool align_dragging = false;  // 是否正在拖拽文字区虚线框
  int align_drag_ox = 0;        // 拖拽起点：光标相对虚线框左上偏移（预览坐标）
  int align_drag_oy = 0;
  int align_drag_sx = 0;        // 拖拽起点 region（图片坐标）
  int align_drag_sy = 0;
  // 文字区「生效值」：text_region 已设置时 = 配置值；未设置（全 0）时 = 默认推导
  // （150/40/图宽-300/图高-80，与候选窗 ResolveTextRegion 一致）——保证虚线框
  // 始终可见可拖拽（用户反馈「红色框根本拖动不了」根因：text_region 全 0 时
  // 虚线框 rw/rh < 1 未绘制、也未命中）。
  int align_eff_x = 0, align_eff_y = 0, align_eff_w = 0, align_eff_h = 0;
  bool align_syncing = false;   // 编辑框 → region 同步中（防 EN_CHANGE 递归）

  // ---- 卡片矩形（客户区坐标，RelayoutControls 每次布局时更新）----
  // WM_CTLCOLORBTN 按按钮中心所在区域返回卡片色/面板色刷子擦圆角外背景：
  // 卡片内按钮（加载/删除/保存、选择字体、角色/输入框、选择图片…）圆角外
  // 露卡片色；面板上按钮（恢复默认/保存并应用/关闭）露面板色——消除四角色差
  // "死角"（2026-10-22：此前统一返回面板色 → 卡片内按钮四角露面板色方块）。
  RECT card_rects[4]{};

  // ---- v4.2 预览位图缓存 ----
  // resize 拖动时 WM_PAINT 连续触发，此前每帧 Gdiplus::Bitmap 从磁盘加载 →
  // IO 慢/失败 → 预览图消失（用户反馈「拉长窗口后上传图片和对齐预览都消失」）。
  // 缓存按皮肤路径失效重建（上传新图/换模式时路径变化自动重建）。
  Gdiplus::Bitmap* preview_cache = nullptr;   // 皮肤预览位图（缩略/对齐共用）
  std::string preview_cache_path;             // 已缓存位图的皮肤路径（utf8）
};

// GDI+ 启动（进程内单例；与 candidate_window 的 RAII 同模式，多实例无冲突）
class GdiplusInit {
 public:
  GdiplusInit() { Gdiplus::GdiplusStartup(&token_, &input_, nullptr); }
  ~GdiplusInit() { Gdiplus::GdiplusShutdown(token_); }

 private:
  Gdiplus::GdiplusStartupInput input_;
  ULONG_PTR token_ = 0;
};
GdiplusInit g_gdiplus_init;

// ---- UTF-8 <-> 宽字符（面板显示与 theme.yaml 落盘共用） ----
std::wstring ToWide(const std::string& utf8) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (n <= 1) {
    return L"";
  }
  std::wstring out(static_cast<std::size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], n);
  return out;
}

std::string ToUtf8(const std::wstring& ws) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) {
    return "";
  }
  std::string out(static_cast<std::size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &out[0], n, nullptr, nullptr);
  return out;
}

// 皮肤图片绝对路径解析（与 candidate_window.cpp ResolveSkinPath 同语义）：
// theme.yaml 存相对文件名（如 "role.png"），面板预览的当前工作目录是 exe
// 所在目录而非配置目录，相对名直接加载 Bitmap 失败 → 上传后无预览
// （真机根因 2026-10-03）。先试原串，再拼 config_dir。
std::wstring ResolveSkinPath(const std::string& rel, const std::string& config_dir) {
  if (rel.empty()) {
    return L"";
  }
  const std::wstring direct = ToWide(rel);
  if (!direct.empty()) {
    Gdiplus::Bitmap probe(direct.c_str());
    if (probe.GetLastStatus() == Gdiplus::Ok) {
      return direct;
    }
  }
  if (!config_dir.empty()) {
    std::string joined = config_dir;
    if (!joined.empty() && joined.back() != '\\' && joined.back() != '/') {
      joined += '\\';
    }
    joined += rel;
    const std::wstring via_dir = ToWide(joined);
    if (!via_dir.empty()) {
      Gdiplus::Bitmap probe(via_dir.c_str());
      if (probe.GetLastStatus() == Gdiplus::Ok) {
        return via_dir;
      }
    }
  }
  return direct;
}

// 2026-10-22 问题3：自定义取色器（替换系统 CHOOSECOLORW，实现见文件末尾
// RunColorPicker/ColorPickerProc——简洁版 RGB 编辑 + 预览 + 色板 + 吸管取色）。
// 返回 true = 用户点「确定」，target 已写入选定的颜色（0xAARRGGBB）。
bool RunColorPicker(HWND owner, std::uint32_t* target);

// ---- GDI+ 圆角绘制（自绘卡片/按钮/色块共用） ----
void FillRoundRect(Graphics& g, int x, int y, int w, int h, int r, std::uint32_t argb) {
  if (w <= 0 || h <= 0) {
    return;
  }
  // DPI 缩放：坐标/尺寸统一放大（g_dpi_scale）。调用方传逻辑像素，内部转换。
  x = DS(x);
  y = DS(y);
  w = DS(w);
  h = DS(h);
  r = DS(r);
  int rr = r;
  if (rr < 0) rr = 0;
  if (rr > w / 2) rr = w / 2;
  if (rr > h / 2) rr = h / 2;
  if (rr == 0) {
    // 圆角为 0 = 普通矩形：必须走 FillRectangle。AddArc 零尺寸矩形会生成
    // 退化路径，FillPath 空操作 → 背景未绘制 → 客户区残留窗口创建时屏幕
    // 内容（2026-10-22 实锤：取色窗口面板底 r=0 未画，用户实测"透明底、
    // 父级内容透出、拖动时文字跟着走、滑块时闪动白屏"）。
    SolidBrush br{Color(argb)};
    g.FillRectangle(&br, x, y, w, h);
    return;
  }
  GraphicsPath path;
  path.AddArc(x, y, rr * 2, rr * 2, 180, 90);
  path.AddArc(x + w - rr * 2, y, rr * 2, rr * 2, 270, 90);
  path.AddArc(x + w - rr * 2, y + h - rr * 2, rr * 2, rr * 2, 0, 90);
  path.AddArc(x, y + h - rr * 2, rr * 2, rr * 2, 90, 90);
  path.CloseFigure();
  SolidBrush br{Color(argb)};
  g.FillPath(&br, &path);
}

void DrawRoundRect(Graphics& g, int x, int y, int w, int h, int r, std::uint32_t argb,
                   float pen_w = 1.0f) {
  if (w <= 0 || h <= 0) {
    return;
  }
  x = DS(x);
  y = DS(y);
  w = DS(w);
  h = DS(h);
  r = DS(r);
  pen_w *= g_dpi_scale;  // 描边宽随缩放
  int rr = r;
  if (rr < 0) rr = 0;
  if (rr > w / 2) rr = w / 2;
  if (rr > h / 2) rr = h / 2;
  if (rr == 0) {
    Pen pen(Color(argb), pen_w);
    g.DrawRectangle(&pen, x, y, w, h);
    return;
  }
  GraphicsPath path;
  path.AddArc(x, y, rr * 2, rr * 2, 180, 90);
  path.AddArc(x + w - rr * 2, y, rr * 2, rr * 2, 270, 90);
  path.AddArc(x + w - rr * 2, y + h - rr * 2, rr * 2, rr * 2, 0, 90);
  path.AddArc(x, y + h - rr * 2, rr * 2, rr * 2, 90, 90);
  path.CloseFigure();
  Pen pen(Color(argb), pen_w);
  g.DrawPath(&pen, &path);
}

// 文字绘制（GDI+ DrawString；align 0=左 1=中 2=右）
// cxw：居中/右对齐的基准宽度（物理像素）。传入时用 cxw 而非固定 2000，
// 否则放大窗口（DPI>100%）下文字中心仍按 1000 算，被推到窗口外/边缘，
// 造成"父级面板放大后文字错位"。默认 0=旧行为（2000）。
void DrawTextC(Graphics& g, const wchar_t* text, const wchar_t* family, float size, int x,
               int y, std::uint32_t argb, bool bold = false, int align = 0, int cxw = 0) {
  const FontStyle style = bold ? FontStyleBold : FontStyleRegular;
  // DPI 缩放：字号与坐标统一放大。调用方传逻辑像素。
  size *= g_dpi_scale;
  x = DS(x);
  y = DS(y);
  Font font(family, size, style, UnitPixel);
  SolidBrush br{Color(argb)};
  StringFormat fmt;
  if (align == 1) {
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
  } else if (align == 2) {
    fmt.SetAlignment(Gdiplus::StringAlignmentFar);
  }
  const float rw = (cxw > 0) ? static_cast<float>(cxw) : 2000.0f;
  g.DrawString(text, -1, &font,
               RectF(static_cast<float>(x), static_cast<float>(y), rw, 200.0f), &fmt,
               &br);
}

// ---- 控件创建 ----
HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h, int id = 0,
                 DWORD extra_style = 0) {
  return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | extra_style, x, y, w, h,
                         parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandleW(nullptr), nullptr);
}

// 自绘按钮：BS_OWNERDRAW（DrawItem 画圆角；系统自动提供 hover/按下状态）
HWND CreateOwnButton(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h) {
  return CreateWindowExW(0, L"BUTTON", text,
                         WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_PUSHBUTTON, x, y, w, h,
                         parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandleW(nullptr), nullptr);
}

// 色块：自绘按钮（DrawItem 画圆角色块），点击发 BN_CLICKED
HWND CreateSwatch(HWND parent, int id, int x, int y, int w, int h) {
  return CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, x, y, w,
                         h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                         GetModuleHandleW(nullptr), nullptr);
}

// 编辑框圆角自绘（2026-10-10）：原窗口过程存于属性 kOrigEditProcProp。
const wchar_t kOrigEditProcProp[] = L"RimeOrigEditProc";
const wchar_t kOrigComboProcProp[] = L"RimeOrigComboProc";
LRESULT CALLBACK RoundedEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// v4.4：COMBOBOX 子类化——深色模式下系统主题默认白底直角突兀。拦截
// WM_ERASEBKGND 用 edit_bg 色填满客户区（主体背景），下拉列表由
// WM_CTLCOLORLISTBOX 统一深色。下拉箭头按钮仍由系统画（小面积可接受）。
LRESULT CALLBACK ComboBoxProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == WM_ERASEBKGND) {
    HDC dc = reinterpret_cast<HDC>(wParam);
    RECT r{};
    GetClientRect(hwnd, &r);
    FillRect(dc, &r, GetEditBrush());
    return 1;
  }
  // v4.4：WM_PAINT 也先画深色底——经典样式 COMBOBOX 在 WM_PAINT 里自画背景，
  // 不走 WM_ERASEBKGND；先 GetDC 填 edit_bg，再 CallWindowProc 画文本。
  if (msg == WM_PAINT) {
    HDC dc = GetDC(hwnd);
    if (dc != nullptr) {
      RECT r{};
      GetClientRect(hwnd, &r);
      FillRect(dc, &r, GetEditBrush());
      ReleaseDC(hwnd, dc);
    }
  }
  const auto orig = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kOrigComboProcProp));
  return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
}

HWND CreateEdit(HWND parent, int id, int x, int y, int w, bool numeric = true) {
  // 无边框 EDIT：白卡片底上自然融合（颜色/字体/布局分区）
  // v4.3：numeric=false 时不加 ES_NUMBER——配置名编辑框需输中文/字母，
  // ES_NUMBER 只允许数字（真机反馈「输入框都输入不了东西」根因之一）。
  DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL;
  if (numeric) {
    style |= ES_NUMBER;
  }
  HWND edit = CreateWindowExW(0, L"EDIT", L"", style, x, y, w, 24, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                              GetModuleHandleW(nullptr), nullptr);
  // 2026-10-10 编辑框「白色很方的方框」修复：子类化 EDIT 自绘圆角底（5px 圆角 +
  // 1px 描边）。EDIT 背景刷已设为 NULL_BRUSH 透明（WM_CTLCOLOREDIT）→ 原 EDIT
  // 画文本/光标时不动背景，圆角底保持；系统默认深色下 EDIT 白底直角是突兀根因。
  if (edit != nullptr) {
    const auto orig = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RoundedEditProc)));
    SetPropW(edit, kOrigEditProcProp, reinterpret_cast<HANDLE>(orig));
  }
  return edit;
}

// 编辑框圆角自绘窗口过程（见 CreateEdit）
LRESULT CALLBACK RoundedEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  // v4.4：禁止系统擦除背景——原 EDIT 在 BeginPaint 时用窗口类默认 brush
  // （深色下为残色）擦客户区，会盖掉我们画的圆角底（真机反馈框内偏暗、
  // 圆角外四角露浅灰角标）。返回 1 = 已擦除，让系统不再擦。
  if (msg == WM_ERASEBKGND) {
    return 1;
  }
  if (msg == WM_PAINT) {
    // 先画圆角底（GetDC 直接画，不动更新区），再标记不擦背景让原 EDIT 画
    // 文本/光标（背景透明 → 圆角底保持）。
    HDC dc = GetDC(hwnd);
    if (dc != nullptr) {
      RECT r{};
      GetClientRect(hwnd, &r);
      Gdiplus::Graphics g(dc);
      g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      const int w = r.right - r.left, h = r.bottom - r.top;
      FillRoundRect(g, 0, 0, w, h, 5, g_pal->edit_bg);
      DrawRoundRect(g, 0, 0, w, h, 5, g_pal->edit_border, 1.0f);
      ReleaseDC(hwnd, dc);
    }
    // v2.1：删掉末尾 InvalidateRect(hwnd,nullptr,FALSE)——WM_PAINT 内自激
    // InvalidateRect = 重绘循环（队列一空就再发 WM_PAINT），消息队列被持续
    // 灌满，退格等按键消息被饿死/延迟（真机「数值框退格删不掉」根因之一）。
    // 内容变化时 EDIT 自己会 InvalidateRect caret/行区，无需自激。
  }
  const auto orig = reinterpret_cast<WNDPROC>(GetPropW(hwnd, kOrigEditProcProp));
  // v2.1 退格修复：标准 EDIT 的 WM_CHAR 0x08 删除在本子类化（自绘圆角底 +
  // 模态消息循环）环境下不稳定——实测 WM_KEYDOWN/WM_CHAR 全链路到达并转发
  // orig 后字符仍不删。改用官方编辑控件消息 EM_GETSEL/EM_SETSEL/EM_REPLACESEL
  // 做确定性删除（程序化删除的标准 API），WM_CHAR 0x08 一律吞掉，防止
  // TranslateMessage 生成的后续字符再删一次（双删）。
  if (msg == WM_KEYDOWN && wParam == VK_BACK) {
    // 退格 = 删除光标前一字符（Ctrl+Backspace 在本控件也按单字符删除——
    // 数值框只有 1~3 位数字，整词删除无意义；统一走程序化删除，避免依赖
    // orig 黑盒行为导致 Ctrl+Backspace 在 WM_CHAR 被吞后失效）。
    const LRESULT sel = SendMessageW(hwnd, EM_GETSEL, 0, 0);
    const DWORD sel_start = LOWORD(sel), sel_end = HIWORD(sel);
    wchar_t bt_log[64] = {};
    GetWindowTextW(hwnd, bt_log, 64);
    ::ime::LogF("[settings] EDIT 退格：sel=%lu,%lu text='%ls'\n", sel_start, sel_end,
                bt_log);
    if (sel_start == sel_end) {
      if (sel_start == 0) {
        ::ime::LogF("[settings] EDIT 退格：光标在开头，无可删\n");
        return 0;  // 光标已在开头，无可删
      }
      // 无选区：先选中光标前一字符再删除
      SendMessageW(hwnd, EM_SETSEL, sel_start - 1, sel_start);
    }
    // v2.1 退格删除：标准 EDIT 删除当前选区必须用 WM_CLEAR——
    // 上一版误用 EM_REPLACESEL（那是 RichEdit 的消息，普通 EDIT 不处理，
    // 真机实测：只选中前一字符、不删除）。WM_CLEAR = 删除选区不进剪贴板，
    // 正是退格语义。
    SendMessageW(hwnd, WM_CLEAR, 0, 0);
    wchar_t bt_after[64] = {};
    GetWindowTextW(hwnd, bt_after, 64);
    ::ime::LogF("[settings] EDIT 退格删除后 text='%ls'\n", bt_after);
    // v2.1.1 修复「退格后 UI 显示全选/不刷新」：本子类把 WM_ERASEBKGND 置 1
    // （保留圆角底）导致系统不擦背景，WM_CLEAR 删除后原 EDIT 重绘时不擦旧
    // 像素，屏幕残留旧文本+选择高亮，视觉上像「全部选中没删掉」。删除后
    // 显式 InvalidateRect(..., TRUE) 强制整块重绘（含擦背景），让屏幕跟随
    // 真实内容。TRUE=请求擦背景再重绘，RoundedEditProc 的 WM_PAINT 会画
    // 圆角底覆盖旧像素，再由原 EDIT 画新文本。
    ::InvalidateRect(hwnd, nullptr, TRUE);
    return 0;
  }
  if (msg == WM_CHAR && wParam == 0x08) {
    return 0;  // 删除已在 WM_KEYDOWN VK_BACK 完成，吞掉后续 WM_CHAR
  }
  return CallWindowProcW(orig, hwnd, msg, wParam, lParam);
}

int GetEditInt(HWND edit, int fallback, int lo, int hi) {
  wchar_t buf[16] = {};
  GetWindowTextW(edit, buf, 16);
  if (buf[0] == L'\0') {
    return fallback;
  }
  const long v = std::wcstol(buf, nullptr, 10);
  if (v < lo || v > hi) {
    return fallback;
  }
  return static_cast<int>(v);
}

void SetEditInt(HWND edit, int value) {
  wchar_t buf[16];
  swprintf_s(buf, L"%d", value);
  SetWindowTextW(edit, buf);
}

// ChooseFontW 可能返回竖排字体名（"@黑体" 等带 @ 前缀），候选窗绘制时
// CreateThemeFont 已去掉 @ 前缀；面板存储/显示也应一致，避免 @ 字体名
// 被当成无效字体塞进 theme.yaml（真机截图「字体名挤在一起」疑似来源之一）。
std::wstring NormalizeFontFamily(const std::wstring& family) {
  if (!family.empty() && family[0] == L'@') {
    return family.substr(1);
  }
  return family;
}

void RefreshBgLabel(SettingsContext* ctx) {
  using ime::i18n::Str;
  const std::wstring text =
      ctx->draft.background_image.empty()
          ? ime::i18n::Tr(Str::kBackgroundNone)
          : (ime::i18n::Tr(Str::kBackgroundLabel) +
             ToWide(ctx->draft.background_image));
  SetWindowTextW(GetDlgItem(ctx->dlg, kLabelBg), text.c_str());
}

// v3：皮肤模式二选一控件显隐与对齐数值同步（前向声明：RefreshAllControls 调用）
void RefreshSkinModeControls(SettingsContext* ctx);

void RefreshAllControls(SettingsContext* ctx) {
  const std::uint32_t colors[4] = {ctx->draft.colors.background, ctx->draft.colors.text,
                                   ctx->draft.colors.highlight_text,
                                   ctx->draft.colors.highlight_background};
  for (int i = 0; i < 4; ++i) {
    ctx->swatch_colors[i] = colors[i];
  }
  RefreshBgLabel(ctx);
  // v2.1：布局卡（圆角/边距X/间距/行高）已整体移除，数值随 theme.yaml，不再有编辑框。
  RefreshSkinModeControls(ctx);  // v3：radio 自绘态 + 输入框模式控件显隐 + 对齐数值
  InvalidateRect(ctx->dlg, nullptr, TRUE);
  // 2026-10-27 加载预设/恢复默认后预览不全刷修复：色块（kSwatchBg..kSwatchHlBg）
  // 是独立子窗口，由 WM_DRAWITEM 从 swatch_colors 自绘——父窗 InvalidateRect
  // 只失效父窗自身区域，不会触发子窗 WM_PAINT。此前只 InvalidateRect(dlg)，
  // 加载 boo 后色块仍显示 bt 兔 的旧颜色（真机回归）。这里 EnumChildWindows
  // 把全部子控件（自绘色块/按钮/编辑框/下拉框）逐个失效，父窗 + 子窗一起重绘。
  EnumChildWindows(ctx->dlg, [](HWND child, LPARAM) -> BOOL {
    InvalidateRect(child, nullptr, TRUE);
    return TRUE;
  }, 0);
}

// ---- v3 皮肤模式二选一：按 draft.skin.mode 显示/隐藏输入框模式的专属控件 ----
void RefreshSkinModeControls(SettingsContext* ctx) {
  const bool frame = ctx->draft.skin.mode == skin::SkinMode::Frame;
  ShowWindow(GetDlgItem(ctx->dlg, kBtnRoleImage), frame ? SW_HIDE : SW_SHOW);
  ShowWindow(GetDlgItem(ctx->dlg, kBtnFrameImage), frame ? SW_SHOW : SW_HIDE);
  ShowWindow(GetDlgItem(ctx->dlg, kEditTrX), frame ? SW_SHOW : SW_HIDE);
  ShowWindow(GetDlgItem(ctx->dlg, kEditTrY), frame ? SW_SHOW : SW_HIDE);
  ShowWindow(GetDlgItem(ctx->dlg, kEditTrW), frame ? SW_SHOW : SW_HIDE);
  ShowWindow(GetDlgItem(ctx->dlg, kEditTrH), frame ? SW_SHOW : SW_HIDE);
  // 编辑框 ↔ text_region 同步（align_syncing 防 EN_CHANGE 递归）
  ctx->align_syncing = true;
  SetEditInt(GetDlgItem(ctx->dlg, kEditTrX), ctx->draft.skin.text_region.x);
  SetEditInt(GetDlgItem(ctx->dlg, kEditTrY), ctx->draft.skin.text_region.y);
  SetEditInt(GetDlgItem(ctx->dlg, kEditTrW), ctx->draft.skin.text_region.width);
  SetEditInt(GetDlgItem(ctx->dlg, kEditTrH), ctx->draft.skin.text_region.height);
  ctx->align_syncing = false;
  InvalidateRect(ctx->dlg, nullptr, TRUE);
}

// 编辑框数值 → text_region（拖拽/保存共用；保存时也校验夹紧）。
// v4.7.5：width/height 的 fallback/min 钳制从 1 改为 0——原钳制在编辑框值为
// 0（未设置，走默认推导）时被 GetEditInt 夹紧成 1 并落盘，候选窗 ResolveTextRegion
// 把 1×1px 当有效文字区 → 文字区塌缩（真机根因）；0 保持 0，与候选窗
// ResolveTextRegion「width/height ≤1 视为未设置」的防御配套。
void SyncTextRegionFromEdits(SettingsContext* ctx) {
  skin::SkinTextRegion& tr = ctx->draft.skin.text_region;
  tr.x = GetEditInt(GetDlgItem(ctx->dlg, kEditTrX), 0, 0, 4096);
  tr.y = GetEditInt(GetDlgItem(ctx->dlg, kEditTrY), 0, 0, 4096);
  tr.width = GetEditInt(GetDlgItem(ctx->dlg, kEditTrW), 0, 0, 4096);
  tr.height = GetEditInt(GetDlgItem(ctx->dlg, kEditTrH), 0, 0, 4096);
}

// ---- 通用对话框（官方用法，见 settings_window.h 头部注释） ----
void PickColor(SettingsContext* ctx, int swatch_id, std::uint32_t* target) {
  // 2026-10-22 自定义取色器：替换系统 CHOOSECOLORW——系统颜色对话框无「吸取
  // 屏幕颜色」能力（用户需求，问题3）。实现见文件末尾 RunColorPicker /
  // ColorPickerProc（简洁版：RGB 编辑 + 预览色块 + 常用色板 + 吸管 GetPixel，
  // 风格与主设置窗口一致）。
  if (RunColorPicker(ctx->dlg, target)) {
    const int idx = swatch_id - kSwatchBg;
    if (idx >= 0 && idx < 4) {
      ctx->swatch_colors[idx] = *target;
      // 关键：必须同步 draft.colors——此前只更新 swatch_colors 用于显示，漏更新
      // draft 导致"保存并应用"用的是旧色（保存走 ctx.draft）→ 真机"取完色新颜色
      // 没生效/候选窗不变色"根因。
      switch (idx) {
        case 0: ctx->draft.colors.background = *target; break;
        case 1: ctx->draft.colors.text = *target; break;
        case 2: ctx->draft.colors.highlight_text = *target; break;
        case 3: ctx->draft.colors.highlight_background = *target; break;
        default: break;
      }
      InvalidateRect(GetDlgItem(ctx->dlg, swatch_id), nullptr, TRUE);  // 色块子控件
      // 2026-10-23 修复：hex 文字是主面板自绘（非色块子控件），只 InvalidateRect
      // 色块不重绘主窗口自绘的 hex → 取色后图二 hex 不更新（用户截图铁证）。
      InvalidateRect(ctx->dlg, nullptr, TRUE);  // 整窗重绘，hex/其它自绘项更新
    }
  }
}

void PickFont(SettingsContext* ctx) {
  LOGFONTW lf{};
  lf.lfHeight = -ctx->draft.font.size;  // 负 = 字符高度（ChooseFont 惯例）
  lf.lfWeight = FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  const std::wstring family = ToWide(ctx->draft.font.family);
  wcsncpy_s(lf.lfFaceName, family.c_str(), _TRUNCATE);
  CHOOSEFONTW cf{};
  cf.lStructSize = sizeof(cf);
  cf.hwndOwner = ctx->dlg;
  cf.lpLogFont = &lf;
  cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;
  if (ChooseFontW(&cf)) {
    ctx->draft.font.family = ToUtf8(NormalizeFontFamily(lf.lfFaceName));
    const long size = std::labs(lf.lfHeight);
    ctx->draft.font.size = static_cast<int>(size < 6 ? 6 : (size > 96 ? 96 : size));
    InvalidateRect(ctx->dlg, nullptr, TRUE);  // 字体名由面板自绘，重绘即可更新
  }
}

void PickBackgroundImage(SettingsContext* ctx) {
  wchar_t file_buf[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = ctx->dlg;
  ofn.lpstrFile = file_buf;
  ofn.lpstrFile[0] = L'\0';
  ofn.nMaxFile = MAX_PATH;
  // 过滤器：双 null 结尾的「描述\0模式\0...\0\0」对
  ofn.lpstrFilter = ime::i18n::TrW(ime::i18n::Str::kFileFilterPng);
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) {
    return;
  }
  const std::wstring src(file_buf);
  // 取文件名（最后一个分隔符之后）
  const std::size_t sep = src.find_last_of(L"\\/");
  const std::wstring name = (sep == std::wstring::npos) ? src : src.substr(sep + 1);
  if (name.empty()) {
    return;
  }
  const std::filesystem::path dst =
      std::filesystem::path(ctx->config_dir) / std::filesystem::path(name);
  // 配置目录缺失时先创建（真机根因 2026-10-03：清理旧配置目录后目录不存在，
  // CopyFileW 直接失败 →「无法把背景图复制到配置目录」）。
  {
    std::error_code ec;
    std::filesystem::create_directories(ctx->config_dir, ec);
    if (ec) {
      const std::wstring msg =
          ime::i18n::Tr(ime::i18n::Str::kErrorCreateConfigDir) +
          ToWide(ctx->config_dir) + L"\n" + ToWide(ec.message());
      MessageBoxW(ctx->dlg, msg.c_str(),
                  ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                  MB_OK | MB_ICONWARNING);
      return;
    }
  }
  // 目标已存在同名文件时也覆盖（用户所选即要用的图）；源==目标时跳过复制。
  // equivalent 在任一路径不存在时会抛 filesystem_error，须 try/catch 兜底。
  const bool same_file = ime::PathsEquivalent(ToUtf8(src), ToUtf8(dst.wstring()));
  if (!same_file && !CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
    const std::wstring msg =
        ime::i18n::Tr(ime::i18n::Str::kErrorCopyBackground) + dst.wstring();
    MessageBoxW(ctx->dlg, msg.c_str(),
                ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                MB_OK | MB_ICONWARNING);
    return;
  }
  ctx->draft.background_image = ToUtf8(name);
  RefreshBgLabel(ctx);
}

void ClearBackgroundImage(SettingsContext* ctx) {
  ctx->draft.background_image.clear();
  RefreshBgLabel(ctx);
}

// v3：上传角色图片（mode=role）/ 上传输入框图片（mode=frame）。
// 流程：GetOpenFileName 选 PNG → 拷贝到 config 目录 → 写入 skin 字段。
void PickSkinImage(SettingsContext* ctx, bool role) {
  wchar_t file_buf[MAX_PATH] = {};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = ctx->dlg;
  ofn.lpstrFile = file_buf;
  ofn.lpstrFile[0] = L'\0';
  ofn.nMaxFile = MAX_PATH;
  // 过滤器：双 null 结尾的「描述\0模式\0...\0\0」对
  ofn.lpstrFilter = ime::i18n::TrW(ime::i18n::Str::kFileFilterPng);
  ofn.nFilterIndex = 1;
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) {
    return;
  }
  const std::wstring src(file_buf);
  const std::size_t sep = src.find_last_of(L"\\/");
  const std::wstring name = (sep == std::wstring::npos) ? src : src.substr(sep + 1);
  if (name.empty()) {
    return;
  }
  // v4.5.2：自动生成新文件名（时间戳），不覆盖旧文件——避免旧图被 rime_server
  // GDI+ Bitmap 文件锁占用导致 CopyFileW 失败（真机反馈「无法把图片复制到配置目录」）。
  // 旧文件保留（rime_server 重启后自动释放），新文件不被占用直接复制成功。
  // v4.5.3：图片存到 skins\ 子目录（config 目录只存配置文件，图片单独存放）。
  const auto now = std::time(nullptr);
  wchar_t new_name[64];
  swprintf_s(new_name, role ? L"role_%lld.png" : L"frame_%lld.png", static_cast<long long>(now));
  const std::filesystem::path skins_dir =
      std::filesystem::path(ctx->config_dir) / L"skins";
  std::error_code ec_dir;
  std::filesystem::create_directories(skins_dir, ec_dir);
  const std::filesystem::path dst = skins_dir / new_name;
  // 配置目录缺失时先创建（同 PickBackgroundImage：清理旧配置目录后目录不存在，
  // CopyFileW 直接失败 →「无法把图片复制到配置目录」）。
  {
    std::error_code ec;
    std::filesystem::create_directories(ctx->config_dir, ec);
    if (ec) {
      const std::wstring msg =
          ime::i18n::Tr(ime::i18n::Str::kErrorCreateConfigDir) +
          ToWide(ctx->config_dir) + L"\n" + ToWide(ec.message());
      MessageBoxW(ctx->dlg, msg.c_str(),
                  ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                  MB_OK | MB_ICONWARNING);
      return;
    }
  }
  const bool same_file = ime::PathsEquivalent(ToUtf8(src), ToUtf8(dst.wstring()));
  bool copied = false;
  if (!same_file) {
    // v4.5：CopyFileW 覆盖失败（旧图被 rime_server 文件锁占用）→ 先删旧文件再复制。
    // 真机反馈「无法把图片复制到配置目录」根因：rime_server 正在加载旧 PNG，
    // CopyFileW(FALSE) 覆盖被共享锁挡住。DeleteFileW 同样可能失败（文件占用），
    // 失败则提示用户重启输入法后重试。
    DeleteFileW(dst.c_str());
    copied = CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
  } else {
    copied = true;
  }
  if (!copied) {
    const std::wstring msg =
        ime::i18n::Tr(ime::i18n::Str::kErrorCopyImage) + dst.wstring() +
        ime::i18n::Tr(ime::i18n::Str::kErrorImageLocked);
    MessageBoxW(ctx->dlg, msg.c_str(),
                ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                MB_OK | MB_ICONWARNING);
    return;
  }
  // 上传前尺寸校验（真机根因 2026-10-04：输入框图 1004×69，九宫格切线
  // top/bottom 40×2=80 超过图高 → 中间源段 0 被跳过，渲染成「中间对半裁开」）。
  // 成熟做法（Android/libGDX NinePatch）：切线必须给中间文字区留足空间。
  // 默认切线 150/150/40/40（theme.h）；此处按图尺寸校验，不合规直接警告、
  // 不接收，让用户重新上传。
  // img_w/img_h 声明提到块外：v4.7.5 换图 text_region 比例迁移（见 frame 分支）
  // 同样要用新图尺寸。
  int img_w = 0, img_h = 0;
  {
    Gdiplus::Bitmap probe(src.c_str());
    if (probe.GetLastStatus() != Gdiplus::Ok) {
      MessageBoxW(ctx->dlg,
                  ime::i18n::TrW(ime::i18n::Str::kErrorDecodeImage),
                  ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                  MB_OK | MB_ICONWARNING);
      return;
    }
    img_w = probe.GetWidth();
    img_h = probe.GetHeight();
    if (role) {
      // 角色图：固定站右侧不变形；建议 256×256，底线 64×64
      if (img_w < 64 || img_h < 64) {
        wchar_t buf[220];
        swprintf_s(buf,
                   ime::i18n::TrW(ime::i18n::Str::kErrorRoleSize),
                   img_w, img_h);
        MessageBoxW(ctx->dlg, buf,
                    ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                    MB_OK | MB_ICONWARNING);
        return;
      }
    } else {
      // 输入框图：九宫格切线 150/150/40/40，中间文字区至少保留 20px
      if (img_w < 320 || img_h < 100) {
        wchar_t buf[280];
        swprintf_s(buf,
                   ime::i18n::TrW(ime::i18n::Str::kErrorFrameSize),
                   img_w, img_h);
        MessageBoxW(ctx->dlg, buf,
                    ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                    MB_OK | MB_ICONWARNING);
        return;
      }
    }
  }
  // v4.5.3：skin 字段存相对 config_dir 的路径（skins\frame_xxx.png），
  // ResolveSkinPath 自动拼接 config_dir + rel 找到文件。
  const std::string rel = std::string(role ? "skins\\role_" : "skins\\frame_") +
                          std::to_string(static_cast<long long>(now)) + ".png";
  if (role) {
    ctx->draft.skin.mode = skin::SkinMode::Role;
    ctx->draft.skin.role_image = rel;
  } else {
    // v4.7.5 换图 text_region 比例迁移（用户反馈「调图时候选框跟随」没了）：
    // v4.6（65b2160d0）把 text_region 从 v4.5（16d0e9f2c）的相对比例(float 0-1)
    // 回退成绝对像素(int)，旧实现换图直接清零 → 选框不再跟随图片。
    // 不回退 v4.6；在绝对像素语义下按「旧图尺寸→新图尺寸」等比重算
    // text_region（保持相对位置）；全 0（未设置）保持 0 走默认推导。
    // v4.5 旧 bug（相对值被当绝对值用 → 错位/黑背景）不得重现：theme.cpp
    // read_int 已把 float<1.0 解析钳制为 0（全 0 走默认），残留的 1×1 塌缩
    // （真机曾被设置窗保存写成 1）由下方 width/height≤1 → 视为未设置清零兜底，
    // 与候选窗 ResolveTextRegion「≤1 一律走默认推导」的防御一致。
    int old_w = 0, old_h = 0;
    if (!ctx->draft.skin.frame_image.empty()) {
      const std::wstring old_path =
          ResolveSkinPath(ctx->draft.skin.frame_image, ctx->config_dir);
      if (!old_path.empty()) {
        Gdiplus::Bitmap old_bmp(old_path.c_str());
        if (old_bmp.GetLastStatus() == Gdiplus::Ok) {
          old_w = old_bmp.GetWidth();
          old_h = old_bmp.GetHeight();
        }
      }
    }
    const skin::SkinTextRegion old_tr = ctx->draft.skin.text_region;  // 按值拷贝（下方就地改写）
    const bool region_set = old_tr.x != 0 || old_tr.y != 0 ||
                            old_tr.width != 0 || old_tr.height != 0;
    const bool region_degenerate = old_tr.width <= 1 || old_tr.height <= 1;
    if (region_set && !region_degenerate && old_w > 0 && old_h > 0) {
      // 等比重算（保持相对位置；四舍五入。值均非负，+0.5 截断即四舍五入）
      ctx->draft.skin.text_region.x =
          static_cast<int>(static_cast<double>(old_tr.x) * img_w / old_w + 0.5);
      ctx->draft.skin.text_region.y =
          static_cast<int>(static_cast<double>(old_tr.y) * img_h / old_h + 0.5);
      ctx->draft.skin.text_region.width =
          static_cast<int>(static_cast<double>(old_tr.width) * img_w / old_w + 0.5);
      ctx->draft.skin.text_region.height =
          static_cast<int>(static_cast<double>(old_tr.height) * img_h / old_h + 0.5);
      // 结果钳制在图内（x+w ≤ 图宽、y+h ≤ 图高；拖拽红框越界值同 ResolveTextRegion
      // 语义，这里先归位再缩放后的重算值，避免新图下选框跑出图片）
      auto clamp_region = [](int& x, int& y, int& w, int& h, int iw, int ih) {
        if (w < 0) w = 0;
        if (h < 0) h = 0;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (w > iw) w = iw;
        if (h > ih) h = ih;
        if (x + w > iw) x = iw - w;
        if (y + h > ih) y = ih - h;
        if (x < 0) x = 0;  // w == iw 时 x 被压回 0
        if (y < 0) y = 0;
      };
      clamp_region(ctx->draft.skin.text_region.x, ctx->draft.skin.text_region.y,
                   ctx->draft.skin.text_region.width,
                   ctx->draft.skin.text_region.height, img_w, img_h);
      // 缩放后仍 ≤1px（异常配置）→ 按未设置处理，整体清零走默认推导
      if (ctx->draft.skin.text_region.width <= 1 ||
          ctx->draft.skin.text_region.height <= 1) {
        ctx->draft.skin.text_region.x = 0;
        ctx->draft.skin.text_region.y = 0;
        ctx->draft.skin.text_region.width = 0;
        ctx->draft.skin.text_region.height = 0;
      }
    } else {
      // 未设置（全 0）/ 异常值（相对比例残留、1×1 塌缩）/ 旧图不可解码：
      // 整体清零，走默认推导 150/40/图宽-300/图高-80
      ctx->draft.skin.text_region.x = 0;
      ctx->draft.skin.text_region.y = 0;
      ctx->draft.skin.text_region.width = 0;
      ctx->draft.skin.text_region.height = 0;
    }
    ctx->draft.skin.mode = skin::SkinMode::Frame;
    ctx->draft.skin.frame_image = rel;
  }
  InvalidateRect(ctx->dlg, nullptr, TRUE);
}

// v2.1：布局卡（圆角/边距X/间距/行高）已整体移除——CollectNumbers 删除，
// 布局数值随 theme.yaml 落盘（面板不再提供自定义 UI）。

bool SaveAndApply(SettingsContext* ctx) {
  // 保存耗时基线（性能定位：用户反馈「保存配置会卡好久」；无测量不结论）
  LARGE_INTEGER _freq, _t0, _t1, _t2, _t3;
  ::QueryPerformanceFrequency(&_freq);
  ::QueryPerformanceCounter(&_t0);
  // v2.1：布局数值框已移除；text_region 仍随保存落盘
  SyncTextRegionFromEdits(ctx);  // v3：文字区域对齐数值随保存落盘
  // v2.1 保存诊断：打印 text_region 实际落盘值（验证「拖红框→保存→候选窗位置」链路）
  ::ime::LogF("[skin] SaveAndApply text_region=(%d,%d,%d,%d)\n",
              ctx->draft.skin.text_region.x, ctx->draft.skin.text_region.y,
              ctx->draft.skin.text_region.width, ctx->draft.skin.text_region.height);
  // v4.5.4：theme.yaml 存回 config 根目录（图片在 skins\ 子目录，frame_image 存
  // "skins\\frame_xxx.png" 相对路径——ResolveSkinPath 拼接 config_dir + rel 正确）
  const auto yaml_path = std::filesystem::path(ctx->config_dir) / "theme.yaml";
  // 防御：写盘路径如遇异常（磁盘满/权限/路径编码）必须捕获，杜绝未捕获异常
  // → std::terminate → 0xc0000409 闪退（真机：保存中文配置名闪退根因）。
  bool yaml_ok = false;
  try {
    yaml_ok = skin::WriteThemeToYaml(yaml_path.string(), ctx->draft);
  } catch (const std::exception& e) {
    ::ime::LogF("[skin] 保存 theme.yaml 抛异常(已捕获): %s\n", e.what());
  }
  if (!yaml_ok) {
    MessageBoxW(ctx->dlg,
                ime::i18n::TrW(ime::i18n::Str::kErrorWriteTheme),
                ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                MB_OK | MB_ICONERROR);
    return false;
  }
  ::QueryPerformanceCounter(&_t1);  // WriteThemeToYaml 完成
  // 2026-10-14 修复（v2.1 收尾）：用户「保存并应用」= 已自定义主题 → 落标记，
  // 服务启动时据此跳过深色模式覆盖、自动读取用户最新保存的配置。
  {
    std::FILE* mark = std::fopen(
        (std::filesystem::path(ctx->config_dir) / "user_theme.saved").string().c_str(),
        "w");
    if (mark != nullptr) {
      std::fputs("saved\n", mark);
      std::fclose(mark);
    }
  }
  ::QueryPerformanceCounter(&_t2);  // user_theme.saved 完成
  ::ime::LogF( "[skin] 设置面板保存并应用（name=%s，背景=%s）\n", ctx->draft.name.c_str(),
              ctx->draft.background_image.empty() ? "(无)" : ctx->draft.background_image.c_str());
  if (ctx->on_apply) {
    ctx->on_apply(ctx->draft);  // 服务端热更新：候选窗下次渲染即换肤
  }
  ::QueryPerformanceCounter(&_t3);
  ::ime::LogF("[skin] 保存耗时(ms): 写yaml=%.1f 写mark=%.1f on_apply=%.1f 总=%.1f\n",
              (_t1.QuadPart - _t0.QuadPart) * 1000.0 / _freq.QuadPart,
              (_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart,
              (_t3.QuadPart - _t2.QuadPart) * 1000.0 / _freq.QuadPart,
              (_t3.QuadPart - _t0.QuadPart) * 1000.0 / _freq.QuadPart);
  ctx->applied = true;
  return true;
}

// ---- v3 配置管理：命名皮肤配置的 保存 / 加载 / 删除 ----
// 存储位置：config_dir/presets/<配置名>.yaml（完整 Theme YAML，复用 theme.cpp 读写）。
// 设计稿 v3 顶部卡片：「配置管理」下拉选择 + 加载/删除 + 输入配置名 + 保存当前配置。
namespace {
std::wstring PresetsDir(const std::string& config_dir) {
  return ToWide((std::filesystem::path(config_dir) / "presets").string());
}

// 枚举 presets/*.yaml，按文件名（去 .yaml）填充下拉；保留当前选中项
void RefreshPresetCombo(SettingsContext* ctx) {
  const HWND combo = GetDlgItem(ctx->dlg, kComboPreset);
  if (combo == nullptr) {
    return;
  }
  // 记住当前选中项，刷新后尽量恢复
  wchar_t cur[256] = {};
  const int cur_sel =
      static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
  if (cur_sel != CB_ERR) {
    SendMessageW(combo, CB_GETLBTEXT, cur_sel,
                 reinterpret_cast<LPARAM>(cur));
  }
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  const std::wstring dir = PresetsDir(ctx->config_dir);
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || entry.path().extension() != L".yaml") {
      continue;
    }
    const std::wstring stem = entry.path().stem().wstring();
    if (!stem.empty()) {
      SendMessageW(combo, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(stem.c_str()));
    }
  }
  if (cur[0] != L'\0') {
    const int idx =
        static_cast<int>(SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                      reinterpret_cast<LPARAM>(cur)));
    if (idx != CB_ERR) {
      SendMessageW(combo, CB_SETCURSEL, idx, 0);
    }
  }
}

// 保存当前草稿为命名配置（先同步编辑框数值）
void SavePreset(SettingsContext* ctx) {
  wchar_t name[128] = {};
  GetDlgItemTextW(ctx->dlg, kEditPresetName, name, 128);
  // 去首尾空白；空名提示
  std::wstring trimmed = name;
  while (!trimmed.empty() && (trimmed.front() == L' ' || trimmed.front() == L'\t')) {
    trimmed.erase(trimmed.begin());
  }
  while (!trimmed.empty() && (trimmed.back() == L' ' || trimmed.back() == L'\t')) {
    trimmed.pop_back();
  }
  if (trimmed.empty()) {
    MessageBoxW(ctx->dlg,
                ime::i18n::TrW(ime::i18n::Str::kErrorPresetName),
                ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  // v2.1：布局卡已移除，CollectNumbers 随卡删除
  SyncTextRegionFromEdits(ctx);
  std::error_code ec;
  std::filesystem::create_directories(PresetsDir(ctx->config_dir), ec);
  if (ec) {
    MessageBoxW(ctx->dlg,
                ime::i18n::TrW(ime::i18n::Str::kErrorCreatePresetDir),
                ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                MB_OK | MB_ICONERROR);
    return;
  }
  const std::wstring path =
      PresetsDir(ctx->config_dir) + L"\\" + trimmed + L".yaml";
  // 2026-10-27 用户明确要求：同名预设已存在时弹"是否覆盖"确认，不再静默覆盖。
  if (std::filesystem::exists(std::filesystem::path(path))) {
    std::wstring msg = trimmed + L" 已存在，是否覆盖？";
    const int over = MessageBoxW(
        ctx->dlg, msg.c_str(),
        ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (over != IDYES) {
      return;
    }
  }
  // 防御：写 presets 中文配置名可能抛异常（已用 u8path 根治路径编码，此处仍
  // 兜底防止任何未捕获异常 → terminate → 闪退，数据丢失）。
  bool preset_ok = false;
  try {
    preset_ok = skin::WriteThemeToYaml(ToUtf8(path), ctx->draft);
  } catch (const std::exception& e) {
    ::ime::LogF("[skin] 保存配置 %ls 抛异常(已捕获): %s\n", trimmed.c_str(), e.what());
  }
  if (!preset_ok) {
    MessageBoxW(ctx->dlg,
                ime::i18n::TrW(ime::i18n::Str::kErrorSavePreset),
                ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                MB_OK | MB_ICONERROR);
    return;
  }
  // 2026-10-14 修复（v2.1 收尾）：保存命名配置 = 用户已自定义主题 → 落标记，
  // 服务启动时自动读取该配置（presets 下最新保存）且不再被深色模式覆盖。
  {
    std::FILE* mark = std::fopen(
        (std::filesystem::path(ctx->config_dir) / "user_theme.saved").string().c_str(),
        "w");
    if (mark != nullptr) {
      std::fputs("saved\n", mark);
      std::fclose(mark);
    }
  }
  RefreshPresetCombo(ctx);
  const HWND combo = GetDlgItem(ctx->dlg, kComboPreset);
  const int idx =
      static_cast<int>(SendMessageW(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
                                    reinterpret_cast<LPARAM>(trimmed.c_str())));
  if (idx != CB_ERR) {
    SendMessageW(combo, CB_SETCURSEL, idx, 0);
  }
  ::ime::LogF("[skin] 配置管理：保存配置 %ls\n", trimmed.c_str());
}

// 加载选中配置到草稿（不写 theme.yaml；需「保存并应用」才生效）
void LoadPreset(SettingsContext* ctx) {
  const HWND combo = GetDlgItem(ctx->dlg, kComboPreset);
  const int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
  if (sel == CB_ERR) {
    MessageBoxW(ctx->dlg,
                ime::i18n::TrW(ime::i18n::Str::kErrorSelectPreset),
                ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  wchar_t name[256] = {};
  SendMessageW(combo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(name));
  const std::wstring path = PresetsDir(ctx->config_dir) + L"\\" + name + L".yaml";
  skin::Theme loaded;
  if (!skin::LoadThemeFromYaml(ToUtf8(path), &loaded)) {
    MessageBoxW(ctx->dlg,
                ime::i18n::TrW(ime::i18n::Str::kErrorLoadPreset),
                ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                MB_OK | MB_ICONERROR);
    return;
  }
  // 与打开窗口时同样的 @ 字体名归一化（防竖排 @ 字体名被判无效）
  if (!loaded.font.family.empty() && loaded.font.family[0] == '@') {
    loaded.font.family = loaded.font.family.substr(1);
  }
  // 路径基准修正：preset 文件在 presets/ 子目录，但 yaml 里图片路径约定为相对
  // config 根（"skins/xxx.png"）。LoadThemeFromYaml 按 preset 所在目录解析会错成
  // presets/skins/xxx.png（不存在）。这里统一转成相对 config 根的相对路径，这样
  // 后续「保存并应用」写入 theme.yaml 后，server 按 config 根解析正确。
  {
    const auto cfg = std::filesystem::path(ctx->config_dir);
    auto fix_to_config_rel = [&](std::string& p) {
      if (p.empty()) return;
      std::error_code ec;
      auto fp = std::filesystem::path(p).lexically_normal();
      // 若路径里含 \presets\ → 去掉这一层（preset 解析时多拼了 presets/）
      const auto w = fp.wstring();
      const std::wstring marker = L"\\presets\\";
      const auto pos = w.find(marker);
      if (pos != std::wstring::npos) {
        fp = cfg / w.substr(pos + marker.size());
      }
      // 统一取相对 config 根的相对路径（绝对路径也一并规范化）
      const auto rel = std::filesystem::relative(fp, cfg, ec);
      if (!ec && !rel.empty() && rel.is_relative()) {
        p = rel.string();
      }
    };
    fix_to_config_rel(loaded.skin.role_image);
    fix_to_config_rel(loaded.skin.frame_image);
    fix_to_config_rel(loaded.background_image);
  }
  ctx->draft = loaded;
  RefreshAllControls(ctx);
  RefreshSkinModeControls(ctx);
  InvalidateRect(ctx->dlg, nullptr, TRUE);
  ::ime::LogF("[skin] 配置管理：加载配置 %ls\n", name);
}

// 删除选中配置（仅删 presets/ 下文件，不影响 theme.yaml）
void DeletePreset(SettingsContext* ctx) {
  const HWND combo = GetDlgItem(ctx->dlg, kComboPreset);
  const int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
  if (sel == CB_ERR) {
    MessageBoxW(ctx->dlg,
                ime::i18n::TrW(ime::i18n::Str::kErrorSelectPreset),
                ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  wchar_t name[256] = {};
  SendMessageW(combo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(name));
  const std::wstring path = PresetsDir(ctx->config_dir) + L"\\" + name + L".yaml";
  std::error_code ec;
  if (!std::filesystem::remove(std::filesystem::path(path), ec) || ec) {
    MessageBoxW(ctx->dlg,
                ime::i18n::TrW(ime::i18n::Str::kErrorDeletePreset),
                ime::i18n::TrW(ime::i18n::Str::kSettingsTitle),
                MB_OK | MB_ICONERROR);
    return;
  }
  RefreshPresetCombo(ctx);
  ::ime::LogF("[skin] 配置管理：删除配置 %ls\n", name);
}
}  // namespace

// ---- 自绘：面板背景 + 圆角卡片 + 标题 ----
// 皮肤预览位图缓存（v4.2）：按路径缓存，resize 连续重绘不再每帧读磁盘。
Gdiplus::Bitmap* GetPreviewBitmap(SettingsContext* ctx, const std::string& utf8_path) {
  if (ctx->preview_cache != nullptr && ctx->preview_cache_path == utf8_path) {
    return ctx->preview_cache;
  }
  delete ctx->preview_cache;
  ctx->preview_cache = nullptr;
  ctx->preview_cache_path.clear();
  if (!utf8_path.empty()) {
    const std::wstring wpath = ResolveSkinPath(utf8_path, ctx->config_dir);
    if (!wpath.empty()) {
      auto* bmp = new Gdiplus::Bitmap(wpath.c_str());
      if (bmp->GetLastStatus() == Gdiplus::Ok && bmp->GetWidth() > 0 &&
          bmp->GetHeight() > 0) {
        ctx->preview_cache = bmp;
        ctx->preview_cache_path = utf8_path;
        ::ime::LogF("[skin] 预览加载成功：%s → %ls（%dx%d）\n", utf8_path.c_str(),
                    wpath.c_str(), bmp->GetWidth(), bmp->GetHeight());
      } else {
        ::ime::LogF("[skin] 预览加载失败：%s → %ls（GDI+ status=%d）\n",
                    utf8_path.c_str(), wpath.c_str(),
                    static_cast<int>(bmp->GetLastStatus()));
        delete bmp;
      }
    } else {
      ::ime::LogF("[skin] 预览路径解析为空：%s（config_dir=%s）\n", utf8_path.c_str(),
                  ctx->config_dir.c_str());
    }
  } else {
    ::ime::LogF("[skin] 预览路径为空：frame_image 未配置\n");
  }
  return ctx->preview_cache;
}

// 预览缩略：等比缩放贴入虚线框；无图/加载失败时画虚线框 + 提示文字。
// config_dir：theme.yaml 目录，解析相对文件名的皮肤图片（真机根因 2026-10-03：
// 工作目录 ≠ 配置目录，相对路径预览加载失败 → 上传后无预览）。
void PaintPreview(Graphics& g, SettingsContext* ctx, const std::string& utf8_path, int x, int y,
                  int w, int h, const wchar_t* hint, const std::string& config_dir) {
  // 传入为逻辑坐标；DrawRectangle/DrawImage 直接 GDI+（不走缩放封装），须转物理
  const int X = DS(x), Y = DS(y), W2 = DS(w), H2 = DS(h);
  Pen dash{Color(g_pal->preview_border), 1.5f * g_dpi_scale};
  dash.SetDashStyle(DashStyleDash);
  g.DrawRectangle(&dash, X, Y, W2, H2);
  Gdiplus::Bitmap* bmp = GetPreviewBitmap(ctx, utf8_path);
  if (bmp != nullptr) {
    const int img_w = bmp->GetWidth();
    const int img_h = bmp->GetHeight();
    if (img_w > 0 && img_h > 0) {
      int dw = W2 - 2, dh = H2 - 2;
      if (img_w * dh > img_h * dw) {
        dh = img_h * dw / img_w;
      } else {
        dw = img_w * dh / img_h;
      }
      if (dw < 1) dw = 1;
      if (dh < 1) dh = 1;
      const int dx = X + (W2 - dw) / 2;
      const int dy = Y + (H2 - dh) / 2;
      g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
      g.DrawImage(bmp, dx, dy, dw, dh);
      return;
    }
  }
  DrawTextC(g, hint, L"Microsoft YaHei UI", 11.0f, x, y + (h - 15) / 2, g_pal->text_sub, false, 1,
            DS(W2));
}

// 窗口宽度自适应重排：窗口被用户拖边缘放大后，宽度敏感的控件跟随新客户区宽
// 右对齐/等宽重排（放大后内容不破版、不挤在左侧）。垂直方向卡片固定高度，
// 底部按钮带随 client_h 自适应（已有）。初始创建后也调用一次（W=kDlgW 时结果
// 与写死坐标一致，保证「坐标单一来源」不漂移）。
// 垂直布局间隙（v4.3）：卡片高度固定，卡间距随窗口高度扩展——
// 拉高窗口时界面跟着拉伸（此前固定 14px，拉高后中间大片空白）。
// 初始 H=kDlgH 时 gap=15（与 14 差 1px，创建段/绘制段同一公式保证一致）。
int CardGapForHeight(int H) {
  const int fixed_top = 16;
  const int card_h_total = 84 + 156 + 66 + 224 + 96;  // 5 张卡高度
  const int btn_band = 44;
  const int avail = H - fixed_top - card_h_total - btn_band;
  const int gap = avail / 4;
  return gap < 14 ? 14 : gap;
}

void RelayoutControls(SettingsContext* ctx, float scale) {
  HWND dlg = ctx->dlg;
  // DPI：使用调用方传入的权威 scale（WM_DPICHANGED 传 wParam 新 DPI）。
  // 绝不能在此用 GetDpiForWindow(dlg) 重算——跨屏瞬间它常返回旧屏 DPI，把
  // 正确的新 scale 覆盖回旧值 → 控件按旧 scale 摆放、自绘按新 scale 画 → 破版
  //（真机双屏 168 实测：同一次拖拽时好时坏，正是该竞态）。窗口尺寸(lParam 建议
  // 矩形)与内容 scale 必须同源。宽高统一以 GetClientRect 为准，RelayoutControls
  // 与 PaintPanel 坐标完全一致，不依赖 WM_SIZE 的 lParam。
  g_dpi_scale = scale;
  RECT cr{};
  GetClientRect(dlg, &cr);
  const int Wp = cr.right, Hp = cr.bottom;
  const float inv = 1.0f / g_dpi_scale;
  const int Wl = static_cast<int>(static_cast<float>(Wp) * inv + 0.5f);
  const int Hl = static_cast<int>(static_cast<float>(Hp) * inv + 0.5f);
  const int c1x = 14, c1w = Wl - 28;
  const int gap = CardGapForHeight(Hl);
  const int c0y = 16, c0h = 84;
  // 卡片0 配置管理区控件重排（2026-10-24 跨屏补漏）：此前 RelayoutControls 只重排
  // 卡片1~3 与底部按钮，卡片0（下拉/加载/删除/配置名编辑/保存当前配置）只在创建时
  // 按 scale 定位。窗口拖到不同 DPI 副屏后其余卡片按新 scale 放大，卡片0 停在旧 scale
  // → 挤在左上、与 1.75 布局错位破版。此处镜像创建参数 + DS() 一并缩放。
  MoveWindow(GetDlgItem(dlg, kComboPreset), DS(16), DS(c0y + 32), DS(160), DS(200), TRUE);
  MoveWindow(GetDlgItem(dlg, kBtnPresetLoad), DS(16 + 172), DS(c0y + 28), DS(72), DS(28), TRUE);
  MoveWindow(GetDlgItem(dlg, kBtnPresetDelete), DS(16 + 252), DS(c0y + 28), DS(72), DS(28), TRUE);
  MoveWindow(GetDlgItem(dlg, kEditPresetName), DS(16), DS(c0y + 58), DS(160), DS(24), TRUE);
  MoveWindow(GetDlgItem(dlg, kBtnPresetSave), DS(16 + 172), DS(c0y + 54), DS(120), DS(28), TRUE);
  const int c1y = c0y + c0h + gap, c1h = 156;
  const int c2y = c1y + c1h + gap;
  const int c3y = c2y + 66 + gap;
  // 颜色卡色块：左列固定左侧、右列右对齐（距卡片右缘 184 = 26px 块 + 158px 间距，与原布局一致）
  // v4.4：左列色块此前漏重排（只重排了右列），拉高窗口 gap 变大后左列色块悬在卡片1 上方、
  // 与左列自绘文字错位 → 一并补上（x 同创建段 c1x+16）
  MoveWindow(GetDlgItem(dlg, kSwatchBg), DS(c1x + 16), DS(c1y + 46), DS(26), DS(26), TRUE);
  MoveWindow(GetDlgItem(dlg, kSwatchHlText), DS(c1x + 16), DS(c1y + 92), DS(26), DS(26), TRUE);
  const int right_swatch_x = c1x + c1w - 210;
  MoveWindow(GetDlgItem(dlg, kSwatchText), DS(right_swatch_x), DS(c1y + 46), DS(26), DS(26),
              TRUE);
  MoveWindow(GetDlgItem(dlg, kSwatchHlBg), DS(right_swatch_x), DS(c1y + 92), DS(26), DS(26),
              TRUE);
  // 字体按钮：右对齐（96px 宽 + 20px 右距）
  MoveWindow(GetDlgItem(dlg, kBtnFont), DS(c1x + c1w - 116), DS(c2y + 18), DS(96), DS(28),
              TRUE);
  // 皮肤模式 radio：等宽两块（设计稿 .mode-option flex:1 平分，间隙 10px）
  const int radio_w = (c1w - 28 - 10) / 2;
  const int radio_x = c1x + 14;
  MoveWindow(GetDlgItem(dlg, kRadioRole), DS(radio_x), DS(c3y + 46), DS(radio_w), DS(28),
              TRUE);
  MoveWindow(GetDlgItem(dlg, kRadioFrame), DS(radio_x + radio_w + 10), DS(c3y + 46),
              DS(radio_w), DS(28), TRUE);
  // 上传按钮：预览缩略（右侧 88px + 14px 边距）左侧，18px 间隙
  const int upload_x = c1x + c1w - 88 - 14 - 96 - 18;
  MoveWindow(GetDlgItem(dlg, kBtnRoleImage), DS(upload_x), DS(c3y + 84), DS(96), DS(28), TRUE);
  MoveWindow(GetDlgItem(dlg, kBtnFrameImage), DS(upload_x), DS(c3y + 84), DS(96), DS(28), TRUE);
  MoveWindow(GetDlgItem(dlg, kBtnOpenFolder), DS(upload_x + 100), DS(c3y + 84), DS(96),
              DS(28), TRUE);  // v4.5.5
  // 文字区域对齐编辑框（kEditTrX/Y/W/H）：x 同创建段，y 按当前 gap 动态重算；
  // 编辑框显隐由 RefreshAllControls 的 ShowWindow 控制，此处无条件 MoveWindow。
  // v2.1：布局卡（圆角/边距X/间距/行高）已整体移除，不再重排。
  MoveWindow(GetDlgItem(dlg, kEditTrX), DS(16), DS(c3y + 220), DS(44), DS(24), TRUE);
  MoveWindow(GetDlgItem(dlg, kEditTrY), DS(16 + 90), DS(c3y + 220), DS(44), DS(24), TRUE);
  MoveWindow(GetDlgItem(dlg, kEditTrW), DS(16 + 180), DS(c3y + 220), DS(44), DS(24), TRUE);
  MoveWindow(GetDlgItem(dlg, kEditTrH), DS(16 + 270), DS(c3y + 220), DS(44), DS(24), TRUE);
  // 底部按钮带：右对齐（放大窗口后按钮跟随右缘，不挤在左侧）。
  // 相对式验证（W=460）：reset=16/save=160/close=330，与初始写死值一致。
  const int close_x = c1x + c1w - 116;  // 关闭（100 宽 + 16 右距）
  const int save_x = close_x - 170;     // 保存并应用（130 宽 + 40 间距）
  const int reset_x = save_x - 144;     // 恢复默认（100 宽 + 44 间距）
  const int btn_y = Hl - 44;
  MoveWindow(GetDlgItem(dlg, kBtnReset), DS(reset_x), DS(btn_y), DS(100), DS(30), TRUE);
  MoveWindow(GetDlgItem(dlg, kBtnSave), DS(save_x), DS(btn_y), DS(130), DS(30), TRUE);
  MoveWindow(GetDlgItem(dlg, kBtnClose), DS(close_x), DS(btn_y), DS(100), DS(30), TRUE);
  // 卡片矩形（与 PaintPanel 同公式；卡 3 高度取 200 覆盖皮肤模式区按钮）
  ctx->card_rects[0] = {DS(c1x), DS(c0y), DS(c1x + c1w), DS(c0y + c0h)};
  ctx->card_rects[1] = {DS(c1x), DS(c1y), DS(c1x + c1w), DS(c1y + c1h)};
  ctx->card_rects[2] = {DS(c1x), DS(c2y), DS(c1x + c1w), DS(c2y + 66)};
  ctx->card_rects[3] = {DS(c1x), DS(c3y), DS(c1x + c1w), DS(c3y + 200)};
}

void PaintPanel(SettingsContext* ctx, HWND hwnd) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  // DPI：每窗口绘制时现算 scale（跨屏/改缩放后重绘即自适应，不依赖创建时快照）
  g_dpi_scale = GetDpiForWindow(hwnd) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
  RECT cr{};
  GetClientRect(hwnd, &cr);
  // DPI：Wp/Hp=物理像素（双缓冲/背景铺满）；W/H=逻辑像素（布局坐标，
  // 自绘函数 FillRoundRect/DrawTextC 内部按 g_dpi_scale 放大回物理）。
  const int Wp = cr.right, Hp = cr.bottom;
  const float inv = 1.0f / g_dpi_scale;
  const int W = static_cast<int>(static_cast<float>(Wp) * inv + 0.5f);
  const int H = static_cast<int>(static_cast<float>(Hp) * inv + 0.5f);
  // ---- 双缓冲：全部绘制到内存 DC，最后 BitBlt 一次输出 ----
  // 用户反馈拖拽虚线框时「闪眼睛」：此前直接画窗口 DC，WM_MOUSEMOVE 每帧
  // InvalidateRect(TRUE) 触发擦背景+重绘 → 闪烁。标准解法（微软官方 Flicker-Free
  // Displays Using an Off-Screen DC / CSDN 双缓冲一致）：WM_ERASEBKGND 返回 1
  // 禁止擦除，离屏内存 DC 画完整帧后 BitBlt 一次性贴屏。
  HDC mem = CreateCompatibleDC(hdc);
  HBITMAP bmp = CreateCompatibleBitmap(hdc, Wp, Hp);
  HGDIOBJ old_bmp = SelectObject(mem, bmp);
  {
    Graphics g(mem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    // 面板底：跟随系统浅/深色的纯色背景（两色一致，无渐变；布局不变）
    LinearGradientBrush bg{Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(Wp),
                                          static_cast<float>(Hp)),
                           Color(g_pal->bg_top), Color(g_pal->bg_bottom),
                           Gdiplus::LinearGradientModeVertical};
    g.FillRectangle(&bg, Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(Wp),
                                        static_cast<float>(Hp)));
    // 顶部标题
    DrawTextC(g, ime::i18n::TrW(ime::i18n::Str::kSettingsSkinTitle),
              L"Microsoft YaHei UI", 22.0f, 0, 12, g_pal->accent,
              true, 1, Wp);
    const int c1x = 14, c1w = W - 28;
    // v4.3：垂直布局随窗口高度拉伸（卡间距动态，拉高窗口界面跟着拉伸）
    const int gap = CardGapForHeight(H);
    // 卡片0：配置管理（y=16 h=84，设计稿 v3 顶部卡片；v3.2 上移贴顶去掉顶部空白，
    // 用户反馈「最上面为什么还有一段空着的」）
    const int c0y = 16, c0h = 84;
    FillRoundRect(g, c1x, c0y, c1w, c0h, 6, g_pal->card);
    DrawRoundRect(g, c1x, c0y, c1w, c0h, 6, g_pal->card_border);
    DrawTextC(g, ime::i18n::TrW(ime::i18n::Str::kPresetSection),
              L"Microsoft YaHei UI", 12.0f, c1x + 16, c0y + 9,
              g_pal->accent, true);
    // 卡片1：颜色（y=114 h=156；v3.5 卡片间距 10→14、卡高 146→156 拉开呼吸感）
    // 卡片圆角 6px = 设计稿 v3（.card border-radius: 6px；此前 14px 与设计稿不符）
    const int c1y = c0y + c0h + gap, c1h = 156;
    FillRoundRect(g, c1x, c1y, c1w, c1h, 6, g_pal->card);
    DrawRoundRect(g, c1x, c1y, c1w, c1h, 6, g_pal->card_border);
    DrawTextC(g, ime::i18n::TrW(ime::i18n::Str::kColorSection),
              L"Microsoft YaHei UI", 12.0f, c1x + 16, c1y + 9, g_pal->accent, true);
    {
      // v3.1：2×2 两列。左列 = 背景色/高亮文字色，右列 = 正文色/高亮底色。
      // 色块在 (c1x+16|c1x+222, c1y+46|c1y+96) 26×26（设计稿 26×26 圆角 4），
      // 名称在色块右 40px，hex 紧贴名称。
      const wchar_t* names_l[2] = {
          ime::i18n::TrW(ime::i18n::Str::kColorBackground),
          ime::i18n::TrW(ime::i18n::Str::kColorHighlightText)};
      const wchar_t* names_r[2] = {
          ime::i18n::TrW(ime::i18n::Str::kColorText),
          ime::i18n::TrW(ime::i18n::Str::kColorHighlightBackground)};
      const int rows[2] = {46, 92};
      const int name_x_l = c1x + 16 + 26 + 8;    // = c1x + 50
      // 右列文字随卡片宽右对齐（与 RelayoutControls 的右列色块同源：
      // 色块右缘 = c1x + c1w - 184，名称 = 色块右 8px，hex = 名称右 72px）
      const int name_x_r = c1x + c1w - 184 + 8;
      const int hex_x_r = name_x_r + 72;
      for (int r = 0; r < 2; ++r) {
        const int ly = c1y + rows[r];
        // 26px 高色块，文字 y+7 垂直居中（此前 y+5 偏上，用户反馈「色块和文字没对齐」）
        DrawTextC(g, names_l[r], L"Microsoft YaHei UI", 12.0f, name_x_l, ly + 7, g_pal->text);
        DrawTextC(g, names_r[r], L"Microsoft YaHei UI", 12.0f, name_x_r, ly + 7, g_pal->text);
      }
      wchar_t hex_buf[16];
      for (int r = 0; r < 2; ++r) {
        const int ly = c1y + rows[r];
        // 左列 index: 0(背景色), 2(高亮文字色)；右列 index: 1(正文色), 3(高亮底色)
        // 6 位 #RRGGBB：必须 mask 掉高 8 位 alpha（swatch_colors 是 0xAARRGGBB，
        // 不 mask 则 %06X 仍输出 FF… 8 位，用户实测"还是 #FF 开头"铁证）
        const int idx0 = 0 + r * 2;
        swprintf_s(hex_buf, L"#%06X",
                   static_cast<unsigned int>(ctx->swatch_colors[idx0] & 0x00FFFFFF));
        DrawTextC(g, hex_buf, L"Consolas", 11.0f, name_x_l + 48, ly + 7,
                  ctx->hex_hover == idx0 ? g_pal->accent : g_pal->text_sub);
        // 记录 hex 文字区供点击复制（Consolas 11，7 字符约 50px 宽）
        ctx->swatch_hex_rc[idx0] = {DS(name_x_l + 48), DS(ly), DS(name_x_l + 48 + 56),
                                    DS(ly + 24)};
        const int idx1 = 1 + r * 2;
        swprintf_s(hex_buf, L"#%06X",
                   static_cast<unsigned int>(ctx->swatch_colors[idx1] & 0x00FFFFFF));
        DrawTextC(g, hex_buf, L"Consolas", 11.0f, hex_x_r, ly + 7,
                  ctx->hex_hover == idx1 ? g_pal->accent : g_pal->text_sub);
        ctx->swatch_hex_rc[idx1] = {DS(hex_x_r), DS(ly), DS(hex_x_r + 56), DS(ly + 24)};
      }
    }
    // 卡片2：字体（y=202 h=58）。字体名由面板自绘（13px = 设计稿 .font-value；
    // 不用 STATIC 标签——真机截图「字体白条」根因：STATIC 背景色/文字色跟随系统
    // 控件渲染，深浅切换后白底白字不可读，面板自绘与卡片标题同渲染路径，无此问题）
    const int c2y = c1y + c1h + gap, c2h = 66;
    FillRoundRect(g, c1x, c2y, c1w, c2h, 6, g_pal->card);
    DrawRoundRect(g, c1x, c2y, c1w, c2h, 6, g_pal->card_border);
    DrawTextC(g, ime::i18n::TrW(ime::i18n::Str::kFontSection),
              L"Microsoft YaHei UI", 12.0f, c1x + 16, c2y + 9, g_pal->accent, true);
    {
      // 字体名（与「选择字体…」按钮同排，设计稿 font-row：字体名左、按钮右）
      wchar_t font_buf[96];
      swprintf_s(font_buf, L"%ls %d", ToWide(ctx->draft.font.family).c_str(),
                 ctx->draft.font.size);
      DrawTextC(g, font_buf, L"Microsoft YaHei UI", 13.0f, c1x + 16, c2y + 22,
                g_pal->text);
    }
    // 卡片3：皮肤模式（角色图片 / 输入框图片二选一）y=364 h=224（v3.7 再加高、
    // 内部行距继续拉开——用户反馈「中间这部分太窄、元素挤在一起」）。
    // v3 设计稿（settings_ui_preview_v3.html）：两个自绘单选按钮（radio 行由
    // WM_DRAWITEM 画，位置见控件创建）；下方按所选模式显示对应上传区与
    // （输入框模式）文字区域对齐。
    const int c3y = c2y + c2h + gap, c3h = 280;  // v2.1：布局卡移除后加高皮肤模式卡，元素垂直排开
    FillRoundRect(g, c1x, c3y, c1w, c3h, 6, g_pal->card);
    DrawRoundRect(g, c1x, c3y, c1w, c3h, 6, g_pal->card_border);
    DrawTextC(g, ime::i18n::TrW(ime::i18n::Str::kSkinModeSection),
              L"Microsoft YaHei UI", 12.0f, c1x + 16, c3y + 9,
              g_pal->accent, true);
    const bool frame_mode = ctx->draft.skin.mode == skin::SkinMode::Frame;
    // 上传行：按钮（控件）在左中，预览缩略在右侧（同设计稿 v3 布局）
    if (!frame_mode) {
      DrawTextC(g, ime::i18n::TrW(ime::i18n::Str::kRoleHint1),
                L"Microsoft YaHei UI", 11.0f,
                c1x + 16, c3y + 98, g_pal->text_sub);
      DrawTextC(g, ime::i18n::TrW(ime::i18n::Str::kRoleHint2),
                L"Microsoft YaHei UI", 11.0f, c1x + 16,
                c3y + 120, g_pal->text_sub);
      PaintPreview(g, ctx, ctx->draft.skin.role_image, c1x + c1w - 88 - 14, c3y + 120, 88, 56,
                   ime::i18n::TrW(ime::i18n::Str::kPreview), ctx->config_dir);
    } else {
      DrawTextC(g, ime::i18n::TrW(ime::i18n::Str::kFrameHint1),
                L"Microsoft YaHei UI", 11.0f,
                c1x + 16, c3y + 98, g_pal->text_sub);
      PaintPreview(g, ctx, ctx->draft.skin.frame_image, c1x + c1w - 88 - 14, c3y + 120, 88, 44,
                   ime::i18n::TrW(ime::i18n::Str::kPreview), ctx->config_dir);
      // 文字区域对齐：预览整张输入框图 + 红色虚线文字区（可拖拽），数值微调在下。
      DrawTextC(g, ime::i18n::TrW(ime::i18n::Str::kFrameAlignHint),
                L"Microsoft YaHei UI", 11.0f, c1x + 16, c3y + 144, g_pal->text_sub);
      const int apx = c1x + 16, apy = c3y + 168, apw = c1w - 32, aph = 40;
      // 刷新对齐视图状态（供鼠标命中/拖拽换算）——先清空再按当前图计算
      ctx->align_has_img = false;
      ctx->align_preview_rc = {DS(apx), DS(apy), DS(apx + apw), DS(apy + aph)};
      // 预览底（设计稿 align-preview：深 #111 / 浅 #e8e8e8，圆角 4）
      FillRoundRect(g, apx, apy, apw, aph, 4, g_pal->align_bg);
      Pen dash_red{Color(0xFFE81123), 1.5f * g_dpi_scale};
      dash_red.SetDashStyle(DashStyleDash);
      g.DrawRectangle(&dash_red, DS(apx), DS(apy), DS(apw), DS(aph));  // 预览区底框
      Gdiplus::Bitmap* bmp = GetPreviewBitmap(ctx, ctx->draft.skin.frame_image);
      if (bmp != nullptr) {
        const int src_w = bmp->GetWidth(), src_h = bmp->GetHeight();
        if (src_w > 0 && src_h > 0) {
          const float s = static_cast<float>(DS(aph)) / static_cast<float>(src_h);
          const int dw = static_cast<int>(src_w * s);
          const int dx = DS(apx) + (DS(apw) - dw) / 2;   // 物理，水平居中
          const int dy = DS(apy);
          g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
          g.DrawImage(bmp, dx, dy, dw, DS(aph));
          ctx->align_has_img = true;
          ctx->align_img_x = dx;
          ctx->align_img_y = dy;
          ctx->align_img_w = dw;
          ctx->align_img_h = DS(aph);
          ctx->align_scale = s;
          // 文字区虚线框（text_region → 预览坐标）。未设置（全 0）时用默认推导
          // （与候选窗 ResolveTextRegion 一致），保证虚线框始终可见可拖拽。
          const skin::SkinTextRegion& tr = ctx->draft.skin.text_region;
          int ex = tr.x, ey = tr.y, ew = tr.width, eh = tr.height;
          if (ew <= 0 || eh <= 0) {
            // v2.1 修复：默认推导改用边框切线（与候选窗 ResolveTextRegion 的
            // 默认一致），红框默认位置 = 文字实际默认位置；原硬编码 150/40
            // 与切线脱节，红框与渲染基准不一致。
            ex = ctx->draft.skin.borders.left;
            ey = ctx->draft.skin.borders.top;
            ew = src_w - ctx->draft.skin.borders.left - ctx->draft.skin.borders.right;
            eh = src_h - ctx->draft.skin.borders.top - ctx->draft.skin.borders.bottom;
            if (ew < 1) ew = 1;
            if (eh < 1) eh = 1;
          }
          // v4.3 图内钳制：配置越界会让虚线框画在图外 → 命中失败拖不动
          if (src_w > 0 && ew > 0 && ex + ew > src_w) ex = src_w - ew;
          if (src_h > 0 && eh > 0 && ey + eh > src_h) ey = src_h - eh;
          if (ex < 0) ex = 0;
          if (ey < 0) ey = 0;
          ctx->align_eff_x = ex;
          ctx->align_eff_y = ey;
          ctx->align_eff_w = ew;
          ctx->align_eff_h = eh;
          // 编辑框同步显示像素值
          ctx->align_syncing = true;
          SetEditInt(GetDlgItem(ctx->dlg, kEditTrX), ex);
          SetEditInt(GetDlgItem(ctx->dlg, kEditTrY), ey);
          SetEditInt(GetDlgItem(ctx->dlg, kEditTrW), ew);
          SetEditInt(GetDlgItem(ctx->dlg, kEditTrH), eh);
          ctx->align_syncing = false;
          const int rx = dx + static_cast<int>(ex * s);
          const int ry = dy + static_cast<int>(ey * s);
          const int rw = static_cast<int>(ew * s);
          const int rh = static_cast<int>(eh * s);
          if (rw >= 1 && rh >= 1) {
            g.DrawRectangle(&dash_red, rx, ry, rw, rh);
          }
        }
      }
      // 对齐数值微调 label（X / Y / 宽 / 高，在编辑框正上方）
      const int tr_edits[4][2] = {{16, 0}, {16 + 90, 0}, {16 + 180, 0}, {16 + 270, 0}};
      const wchar_t* tr_labels[4] = {
          ime::i18n::TrW(ime::i18n::Str::kAxisX),
          ime::i18n::TrW(ime::i18n::Str::kAxisY),
          ime::i18n::TrW(ime::i18n::Str::kAxisWidth),
          ime::i18n::TrW(ime::i18n::Str::kAxisHeight)};
      for (int i = 0; i < 4; ++i) {
        DrawTextC(g, tr_labels[i], L"Microsoft YaHei UI", 10.0f, tr_edits[i][0], c3y + 218,
                  g_pal->text_sub, false, 1);
      }
    }
    // v2.1：布局卡（圆角/边距X/间距/行高）整体移除——布局数值随 theme.yaml，
    // 面板不再提供自定义 UI（用户拍板）。
  }
  // 双缓冲：一次性贴屏（无闪烁）
  // 2026-10-24 跨屏根因：画布按物理尺寸 CreateCompatibleBitmap(Wp,Hp)，但此前
  // BitBlt 用逻辑尺寸 W,H（=Wp/scale）。1.0 时 W==Wp 正常；1.75 时只贴左上
  // 462x750，右侧/下方大片露默认白底（用户实拍"黑底只剩1/4"），颜色卡右列文字
  // （x>W）也被裁掉。必须用物理 Wp,Hp 贴满整个客户区。
  BitBlt(hdc, 0, 0, Wp, Hp, mem, 0, 0, SRCCOPY);
  SelectObject(mem, old_bmp);
  DeleteObject(bmp);
  DeleteDC(mem);
  EndPaint(hwnd, &ps);
}

// ---- 自绘按钮/色块（WM_DRAWITEM） ----
void PaintOwnButton(LPDRAWITEMSTRUCT dis, SettingsContext* ctx) {
  g_dpi_scale = GetDpiForWindow(dis->hwndItem) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
  const int id = static_cast<int>(dis->CtlID);
  const bool down = (dis->itemState & ODS_SELECTED) != 0;
  const bool hot = (dis->itemState & ODS_HOTLIGHT) != 0;
  const int x = dis->rcItem.left, y = dis->rcItem.top;
  const int w = dis->rcItem.right - x, h = dis->rcItem.bottom - y;
  if (id >= kSwatchBg && id <= kSwatchHlBg) {
    // 色块：32bpp ARGB 内存位图透明底画圆角矩形，再合成到窗口 DC。
    // 圆角外保持透明、露出卡片底色——修复深色模式下圆角外透出系统按钮
    // 浅灰底（白边）的问题（真机反馈 2026-10-10；此前直接画窗口 DC，
    // BUTTON 控件背景已被系统填充成浅色，圆角 4 角透出白边）。
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    {
      Graphics bg(&bmp);
      bg.SetSmoothingMode(SmoothingModeAntiAlias);
      bg.Clear(Color(0, 0, 0, 0));  // 透明底
      const std::uint32_t c = ctx->swatch_colors[id - kSwatchBg];
      // 设计稿 .color-swatch：26×26 圆角 4px、1px 边框
      FillRoundRect(bg, 0, 0, w, h, 4, c);
      DrawRoundRect(bg, 0, 0, w, h, 4, down ? g_pal->accent : g_pal->swatch_border, 1.0f);
    }
    Graphics g(dis->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.DrawImage(&bmp, x, y, w, h);  // GDI+ 自动 alpha 合成到窗口 DC
    return;
  }
  wchar_t text[64] = {};
  GetWindowTextW(dis->hwndItem, text, 64);
  // 简洁系统风格：所有按钮（含保存并应用）统一灰白底圆角按钮——浅色模式
  // 灰白底(F0F0F0)+深灰字+浅灰描边，深色模式深灰底(3C3C3C)+白字+深灰描边
  // （用户 2026-10-10 明确要求「普通的灰白底圆角按钮」，不再区分主/次配色）。
  // v3 皮肤模式单选（角色/输入框）：选中态 = 整块反色（浅色黑底白字 / 深色
  // 白底黑字，用户 2026-10-10「浅色改黑底白字效果不错、深色对应白底黑字」），
  // 无描边；未选中态 = 普通灰白底/深灰底按钮。
  const bool is_radio = (id == kRadioRole || id == kRadioFrame);
  const bool radio_selected =
      is_radio && ((id == kRadioRole) ? (ctx->draft.skin.mode != skin::SkinMode::Frame)
                                      : (ctx->draft.skin.mode == skin::SkinMode::Frame));
  const bool accent = radio_selected;
  std::uint32_t fill;
  if (accent) {
    fill = g_pal->btn_primary;  // 浅色=黑(1A1A1A)/深色=白(FFFFFF)——反色选中
    if (down) fill = g_pal->btn_primary_down;
    else if (hot) fill = g_pal->btn_primary_hover;
  } else {
    fill = g_pal->btn_secondary;
    if (down) fill = g_pal->btn_secondary_down;
    else if (hot) fill = g_pal->btn_secondary_hover;
  }
  const std::uint32_t text_color = accent ? g_pal->btn_primary_text : g_pal->btn_secondary_text;
  // 32bpp ARGB 内存位图（GDI+）+ DrawImage：GDI+ 对普通窗口 DC 自动做 per-pixel
  // alpha 合成（等效 AlphaBlend）。不依赖 msimg32.h/msimg32.lib——CI 环境
  // （windows-latest + ilammy/msvc-dev-cmd）头文件搜索路径缺 msimg32.h（C1083）。
  Bitmap bmp(w, h, PixelFormat32bppARGB);
  {
    Graphics bg(&bmp);
    bg.SetSmoothingMode(SmoothingModeAntiAlias);
    bg.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    bg.Clear(Color(0, 0, 0, 0));  // 透明底
    // 圆角：普通按钮 10px；radio 选中态 12px（深色白底/浅色黑底整块反色，
    // 更圆润符合视觉参考的胶囊感——用户 2026-10-10「白色边框改圆滑一些」）
    int rr = DS(is_radio && radio_selected ? 12 : 10);
    if (rr > w / 2) rr = w / 2;
    if (rr > h / 2) rr = h / 2;
    GraphicsPath path;
    path.AddArc(0, 0, rr * 2, rr * 2, 180, 90);
    path.AddArc(w - rr * 2, 0, rr * 2, rr * 2, 270, 90);
    path.AddArc(w - rr * 2, h - rr * 2, rr * 2, rr * 2, 0, 90);
    path.AddArc(0, h - rr * 2, rr * 2, rr * 2, 90, 90);
    path.CloseFigure();
    SolidBrush fill_br{Color(0xFF000000u | fill)};  // 不透明纯色
    bg.FillPath(&fill_br, &path);
    // 描边：普通按钮 1px btn_secondary_border（浅色浅灰/深色暗灰，不抢眼）；
    // 选中（radio）整块黑底无描边（用户「黑边奇怪」→ 去掉）
    if (!accent) {
      Pen pen{Color(0xFF000000u | g_pal->btn_secondary_border), 1.0f};
      bg.DrawPath(&pen, &path);
    }
    // 字号：底部按钮 13px（设计稿 .btn），小按钮/单选 12px（.btn-sm / .mode-option）
    const float font_size =
        (id == kBtnSave || id == kBtnReset || id == kBtnClose) ? 13.0f : 12.0f;
    Font font(L"Microsoft YaHei UI", font_size * g_dpi_scale, FontStyleRegular, UnitPixel);
    SolidBrush text_br{Color(0xFF000000u | text_color)};
    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentCenter);
    fmt.SetLineAlignment(StringAlignmentCenter);
    bg.DrawString(text, -1, &font, RectF(0.0f, 0.0f, static_cast<float>(w),
                                         static_cast<float>(h)),
                  &fmt, &text_br);
  }
  {
    Graphics g(dis->hDC);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.DrawImage(&bmp, x, y, w, h);  // GDI+ 自动 alpha 合成到窗口 DC
  }
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* ctx = reinterpret_cast<SettingsContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_PAINT:
      if (ctx != nullptr) {
        PaintPanel(ctx, hwnd);
      }
      return 0;
    case WM_ERASEBKGND:
      // 2026-10-27 回归修复（取色窗/设置窗拖动闪烁）：直接返回 1 禁止擦除，
      // 双缓冲整帧覆盖（微软官方 Flicker-Free Displays Using an Off-Screen DC
      // 标准）。2026-10-24 跨屏修复在此 FillRect 裸画底色——拖动/吸管每帧
      // InvalidateRect(TRUE) 时，该步骤在双缓冲之外直接写屏 DC，与随后 WM_PAINT
      // 整帧之间存在采样窗口，快速拖动露纯底色帧 → 色盘闪动（用户回归）。
      // 去掉后跨屏不闪白：类背景 hbrBackground=nullptr（系统不自动擦白），
      // 过渡间隙露旧帧而非白帧，WM_PAINT 整帧贴上。跨屏 DPI 其余逻辑不动。
      return 1;
    case WM_DRAWITEM:
      if (ctx != nullptr) {
        PaintOwnButton(reinterpret_cast<LPDRAWITEMSTRUCT>(lParam), ctx);
      }
      return TRUE;
    case WM_CTLCOLORBTN: {
      // 按钮圆角外 4 角由系统用本刷子擦背景。自绘按钮（BS_OWNERDRAW）同样
      // 会先收到本消息——按按钮中心所在区域选刷：卡片内按钮圆角外须露
      // 卡片色，面板上按钮须露面板色（此前统一面板色 → 卡片内按钮四角
      // 露面板色方块，用户实拍"多余的死角" 2026-10-22）。
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetBkColor(dc, RGB((g_pal->bg_top >> 16) & 0xFF,
                         (g_pal->bg_top >> 8) & 0xFF,
                         g_pal->bg_top & 0xFF));
      HWND btn = reinterpret_cast<HWND>(lParam);
      RECT br{};
      GetWindowRect(btn, &br);
      POINT pt = {(br.left + br.right) / 2, (br.top + br.bottom) / 2};
      ScreenToClient(ctx->dlg, &pt);
      HBRUSH brush = GetPanelBrush();
      if (ctx != nullptr) {
        for (int i = 0; i < 4; i++) {
          if (PtInRect(&ctx->card_rects[i], pt)) {
            brush = GetCardBrush();
            break;
          }
        }
      }
      return reinterpret_cast<LRESULT>(brush);
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      // v4.4：所有静态文本（含 COMBOBOX 内部嵌入式静态控件）深色底白字。
      // 注意 lParam 是嵌入式控件 HWND（其 ID≠kComboPreset），不能靠 ID 判断——
      // 当前界面已无其他真实 STATIC（全部 PaintPanel 自绘），统一返回深色刷安全。
      SetBkColor(dc, RGB((g_pal->edit_bg >> 16) & 0xFF, (g_pal->edit_bg >> 8) & 0xFF,
                         g_pal->edit_bg & 0xFF));
      SetTextColor(dc, RGB((g_pal->text >> 16) & 0xFF, (g_pal->text >> 8) & 0xFF,
                           g_pal->text & 0xFF));
      return reinterpret_cast<LRESULT>(GetEditBrush());
    }
    case WM_CTLCOLORLISTBOX: {
      // v4.4：下拉列表深色底白字（COMBOBOX 展开的选项列表）
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetBkColor(dc, RGB((g_pal->edit_bg >> 16) & 0xFF, (g_pal->edit_bg >> 8) & 0xFF,
                         g_pal->edit_bg & 0xFF));
      SetTextColor(dc, RGB((g_pal->text >> 16) & 0xFF, (g_pal->text >> 8) & 0xFF,
                           g_pal->text & 0xFF));
      return reinterpret_cast<LRESULT>(GetEditBrush());
    }
    case WM_CTLCOLOREDIT: {
      // 2026-10-10 编辑框「白色很方的方框」修复：背景刷 = NULL_BRUSH 透明 +
      // BkMode TRANSPARENT（经典透明 EDIT 做法）→ 露出 PaintPanel 自绘的圆角
      // 灰底；文字色随深浅模式。系统默认深色下 EDIT 仍是白底直角，这是根因。
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB((g_pal->text >> 16) & 0xFF, (g_pal->text >> 8) & 0xFF,
                           g_pal->text & 0xFF));
      return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_SETTINGCHANGE:
      // 系统深浅色切换：实时反转调色板并重绘（跟随系统）
      RefreshPalette();
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      const int code = HIWORD(wParam);
      // v3 皮肤模式二选一：切 mode + 刷新控件显隐/自绘态
      if (ctx != nullptr &&
          (id == kRadioRole || id == kRadioFrame) && code == BN_CLICKED) {
        ctx->draft.skin.mode = (id == kRadioRole) ? skin::SkinMode::Role
                                                   : skin::SkinMode::Frame;
        RefreshSkinModeControls(ctx);
        return 0;
      }
      // v3 文字区域对齐：编辑框数值变化 → text_region → 重绘预览
      if (ctx != nullptr && id >= kEditTrX && id <= kEditTrH && code == EN_CHANGE &&
          !ctx->align_syncing) {
        ctx->align_syncing = true;
        SyncTextRegionFromEdits(ctx);
        ctx->align_syncing = false;
        InvalidateRect(ctx->dlg, nullptr, TRUE);
        return 0;
      }
      if (id >= kSwatchBg && id <= kSwatchHlBg && code == BN_CLICKED && ctx != nullptr) {
        const int idx = id - kSwatchBg;
        std::uint32_t* targets[4] = {&ctx->draft.colors.background,
                                     &ctx->draft.colors.text,
                                     &ctx->draft.colors.highlight_text,
                                     &ctx->draft.colors.highlight_background};
        PickColor(ctx, id, targets[idx]);
        return 0;
      }
      if (id == kBtnFont && code == BN_CLICKED && ctx != nullptr) {
        PickFont(ctx);
        return 0;
      }
      if (id == kBtnBg && code == BN_CLICKED && ctx != nullptr) {
        PickBackgroundImage(ctx);
        return 0;
      }
      if (id == kBtnBgClear && code == BN_CLICKED && ctx != nullptr) {
        ClearBackgroundImage(ctx);
        return 0;
      }
      if (id == kBtnRoleImage && code == BN_CLICKED && ctx != nullptr) {
        PickSkinImage(ctx, true);
        return 0;
      }
      if (id == kBtnFrameImage && code == BN_CLICKED && ctx != nullptr) {
        PickSkinImage(ctx, false);
        return 0;
      }
      if (id == kBtnReset && code == BN_CLICKED && ctx != nullptr) {
        ctx->draft = skin::DefaultTheme();
        RefreshAllControls(ctx);
        return 0;
      }
      if (id == kBtnSave && code == BN_CLICKED && ctx != nullptr) {
        if (SaveAndApply(ctx)) {
          DestroyWindow(hwnd);
        }
        return 0;
      }
      if (id == kBtnClose && code == BN_CLICKED) {
        DestroyWindow(hwnd);
        return 0;
      }
      // v4.5.3：打开图片保存目录（资源管理器）——skins\ 子目录
      if (id == kBtnOpenFolder && code == BN_CLICKED && ctx != nullptr) {
        const std::wstring skins_dir =
            ToWide(ctx->config_dir) + L"\\skins";
        ShellExecuteW(nullptr, L"open", skins_dir.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
      }
      // v3 配置管理：保存 / 加载 / 删除 命名配置
      if (ctx != nullptr && code == BN_CLICKED) {
        if (id == kBtnPresetSave) {
          SavePreset(ctx);
          return 0;
        }
        if (id == kBtnPresetLoad) {
          LoadPreset(ctx);
          return 0;
        }
        if (id == kBtnPresetDelete) {
          DeletePreset(ctx);
          return 0;
        }
      }
      return 0;
    }
    case WM_DPICHANGED: {
      // MSDN 官方标准（learn.microsoft.com/wm-dpichanged）：用 lParam 的建议矩形
      // 通过 SetWindowPos 重设窗口大小/位置，否则窗口在新屏相对周围一切 grow/
      // shrink。关键：内容 scale 必须用 wParam 的「本次新 DPI」——不能再用
      // GetDpiForWindow(hwnd)（跨屏瞬间常返回旧屏 DPI，导致窗口放大而内容仍按
      // 旧 scale → 内容没放大/错位；真机双屏 168 DPI 实测铁证）。窗口尺寸(lParam)
      // 与内容缩放(wParam)同源，彻底对齐。
      const UINT new_dpi = LOWORD(static_cast<DWORD>(wParam));
      g_dpi_scale = new_dpi / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
      const RECT* prc = reinterpret_cast<const RECT*>(lParam);
      RECT cur{};
      GetWindowRect(hwnd, &cur);
      // 2026-10-24 DPI 过渡帧处理：不用 WM_SETREDRAW 冻结——冻结会把 SetWindowPos
      // 新露出的客户区留着不画，那块是空内存（发黑），浅色主题也照样黑（用户实测
      // 切浅色仍深色底，正是冻结引起）。改用 SWP_NOCOPYBITS：禁止系统把旧尺寸内容
      // 拉伸贴到新窗口（这才是过渡瞬间"破版/白底"帧的来源），新露区域由
      // WM_ERASEBKGND 填当前系统主题底色（深浅色自适应），随后 WM_PAINT 整帧覆盖。
      if (prc != nullptr) {
        ::ime::LogF("[settings] WM_DPICHANGED new_dpi=%u set=%dx%d cur=%dx%d\n",
                    new_dpi, prc->right - prc->left, prc->bottom - prc->top,
                    cur.right - cur.left, cur.bottom - cur.top);
        SetWindowPos(hwnd, nullptr, prc->left, prc->top,
                     prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
      } else {
        ::ime::LogF("[settings] WM_DPICHANGED new_dpi=%u no-lparam\n", new_dpi);
      }
      if (ctx != nullptr) {
        // 传 wParam 新 DPI 算出的权威 scale；RelayoutControls 内部不再用
        // GetDpiForWindow 重算（跨屏瞬间返回旧 DPI 的竞态），窗口尺寸与内容同源。
        RelayoutControls(ctx, new_dpi / static_cast<float>(USER_DEFAULT_SCREEN_DPI));
        InvalidateRect(hwnd, nullptr, TRUE);  // 整窗重绘，双缓冲一次贴屏
      }
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_GETMINMAXINFO: {
      // 最小窗口 = kDlgW×kDlgH 客户区（外框含标题栏+边框）。内容为固定像素
      // 布局（垂直卡片固定高），小于此尺寸会破版/按钮被裁。
      // DPI：按当前窗口 dpi 放大最小尺寸（否则高缩放档下窗口可缩得比内容小）
      g_dpi_scale = GetDpiForWindow(hwnd) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
      auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
      RECT frame{0, 0, DS(kDlgW), DS(kDlgH)};
      AdjustWindowRect(&frame, kSettingsStyle, FALSE);
      mmi->ptMinTrackSize.x = frame.right - frame.left;
      mmi->ptMinTrackSize.y = frame.bottom - frame.top;
      return 0;
    }
    case WM_SIZE: {
      // 窗口被拖边缘缩放：宽度敏感控件按新客户区宽重排（右列右对齐/radio 等宽/
      // 底部按钮右对齐）。scale 取窗口当前真实 DPI（此时已稳定在所在屏）。
      if (ctx != nullptr) {
        RelayoutControls(ctx, GetDpiForWindow(hwnd) / static_cast<float>(USER_DEFAULT_SCREEN_DPI));
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    }
    case WM_LBUTTONDOWN: {
      // v3 文字区域对齐：命中虚线框开始拖拽（输入框模式）
      if (ctx != nullptr && ctx->align_has_img) {
        const int mx = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int my = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        const RECT& rc = ctx->align_preview_rc;
        if (mx >= rc.left && mx <= rc.right && my >= rc.top && my <= rc.bottom) {
          const float s = ctx->align_scale;
          const int rx = ctx->align_img_x + static_cast<int>(ctx->align_eff_x * s);
          const int ry = ctx->align_img_y + static_cast<int>(ctx->align_eff_y * s);
          const int rw = static_cast<int>(ctx->align_eff_w * s);
          const int rh = static_cast<int>(ctx->align_eff_h * s);
          if (rw >= 1 && rh >= 1 && mx >= rx && mx <= rx + rw && my >= ry &&
              my <= ry + rh) {
            ctx->align_dragging = true;
            ctx->align_drag_ox = mx - rx;
            ctx->align_drag_oy = my - ry;
            ctx->align_drag_sx = ctx->align_eff_x;
            ctx->align_drag_sy = ctx->align_eff_y;
            SetCapture(hwnd);
            return 0;
          }
        }
      }
      break;
    }
    case WM_MOUSEMOVE: {
      if (ctx != nullptr && ctx->align_dragging) {
        const int mx = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int my = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        const float s = ctx->align_scale;
        // 作用域修复 2026-10-12：tr 声明在 if(s>0) 块内、却在块外被使用
        // （align_eff_x/align_eff_y/编辑框同步），v4.6 回退时声明被误放进块内 →
        // 编译错误（tr 未声明，rime_server 无法构建）。按 v4.5 正确作用域上移声明：
        // tr 是引用，语义不变；s<=0 时块外读取当前值，与 v4.5 行为一致。
        skin::SkinTextRegion& tr = ctx->draft.skin.text_region;
        if (s > 0.0f) {
          // 图片坐标 = (光标 - 图显示左上 - 拖拽偏移) / 缩放
          // v2.1 修复：删掉前面多加的 align_drag_sx/align_drag_sy——光标未动时
          // (mx-img_x-drag_ox)/s 已经等于起始 eff_x，再补 drag_sx 让起点翻倍，
          // 红框一动就跳到约 2 倍位置（真机「一拖就跳到右下角」根因）。
          const int nx = static_cast<int>((mx - ctx->align_img_x - ctx->align_drag_ox) / s);
          const int ny = static_cast<int>((my - ctx->align_img_y - ctx->align_drag_oy) / s);
          tr.x = nx < 0 ? 0 : nx;
          tr.y = ny < 0 ? 0 : ny;
          if (tr.width <= 0) tr.width = ctx->align_eff_w;  // 未设置时先落默认宽高
          if (tr.height <= 0) tr.height = ctx->align_eff_h;
          // 图内钳制：红框不能拖出图片边界（越界会把候选文字画到图外——
          // 真机根因 text_region.y=209 > 图高200「字不在输入框区域内」）
          const int img_w = static_cast<int>(ctx->align_img_w / s);
          const int img_h = static_cast<int>(ctx->align_img_h / s);
          if (tr.width > 0 && img_w > 0 && tr.x + tr.width > img_w) {
            tr.x = img_w - tr.width;
          }
          if (tr.height > 0 && img_h > 0 && tr.y + tr.height > img_h) {
            tr.y = img_h - tr.height;
          }
          if (tr.x < 0) tr.x = 0;
          if (tr.y < 0) tr.y = 0;
        }
        ctx->align_eff_x = tr.x;
        ctx->align_eff_y = tr.y;
        // 编辑框同步（防 EN_CHANGE 递归）
        ctx->align_syncing = true;
        SetEditInt(GetDlgItem(ctx->dlg, kEditTrX), tr.x);
        SetEditInt(GetDlgItem(ctx->dlg, kEditTrY), tr.y);
        ctx->align_syncing = false;
        InvalidateRect(ctx->dlg, nullptr, TRUE);
      }
      // hex 悬停检测：鼠标移到颜色码上变色，提示"点击可复制"
      if (ctx != nullptr && !ctx->align_dragging) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        int hover = -1;
        for (int i = 0; i < 4; i++) {
          if (PtInRect(&ctx->swatch_hex_rc[i], pt)) {
            hover = i;
            break;
          }
        }
        if (hover != ctx->hex_hover) {
          ctx->hex_hover = hover;
          InvalidateRect(ctx->dlg, nullptr, TRUE);
        }
      }
      return 0;
      break;
    }
    case WM_LBUTTONUP: {
      if (ctx != nullptr && ctx->align_dragging) {
        ctx->align_dragging = false;
        ReleaseCapture();
        InvalidateRect(ctx->dlg, nullptr, TRUE);
        return 0;
      }
      // 点击颜色卡 hex 文字 → 复制该色值到剪贴板（用户要求颜色码可复制粘贴）
      if (ctx != nullptr) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        for (int i = 0; i < 4; i++) {
          if (PtInRect(&ctx->swatch_hex_rc[i], pt)) {
            wchar_t hex[16] = {};
            swprintf_s(hex, L"#%06X",
                       static_cast<unsigned int>(ctx->swatch_colors[i] & 0x00FFFFFF));
            if (OpenClipboard(hwnd)) {
              EmptyClipboard();
              const SIZE_T bytes = (wcslen(hex) + 1) * sizeof(wchar_t);
              HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
              if (hg != nullptr) {
                wchar_t* dst = static_cast<wchar_t*>(GlobalLock(hg));
                if (dst != nullptr) {
                  memcpy(dst, hex, bytes);
                  GlobalUnlock(hg);
                  SetClipboardData(CF_UNICODETEXT, hg);
                }
              }
              CloseClipboard();
            }
            ::ime::LogF("[settings] 复制颜色hex %ls\n", hex);
            break;
          }
        }
      }
      break;
    }
    case WM_DESTROY:
      // v4.2：释放预览位图缓存（避免 GDI+ 对象泄漏）
      if (ctx != nullptr) {
        delete ctx->preview_cache;
        ctx->preview_cache = nullptr;
        ctx->preview_cache_path.clear();
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

bool RunSettingsDialog(HWND owner, const std::string& config_dir,
                       std::function<void(const skin::Theme&)> on_apply) {
  // 2026-10-27 崩溃取证：加载预设偶发未捕获 C++ 异常 → std::terminate →
  // __fastfail(FAST_FAIL_FATAL_APP_EXIT=7) → 0xc0000409 静默闪退（真机事件日志
  // 三次 BeautypeDeployer 崩溃同偏移 0x1d2995 全为此路径）。这里注册 terminate
  // 钩子：崩溃前把异常消息写进日志再 abort，杜绝"无日志闪退"，定位根因无需再猜。
  {
    static std::once_flag g_terminate_once;
    std::call_once(g_terminate_once, [] {
      std::set_terminate([] {
        try {
          std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
          ::ime::LogF("[FATAL] 未捕获 C++ 异常: %s\n", e.what());
        } catch (...) {
          ::ime::LogF("[FATAL] 未捕获未知异常\n");
        }
        std::abort();
      });
    });
  }
  ime::i18n::ReloadLanguage();
  RefreshPalette();  // 打开时按系统深浅色选调色板（WM_SETTINGCHANGE 实时切换）
  // DPI 缩放：窗口创建前设 scale（基于 owner 所在屏，无 owner 用主屏 dpi）。
  // 声明 Per-Monitor V2 后 GetDpiForWindow 返回真实 dpi，布局按此放大。
  g_dpi_scale = GetDpiForWindow(owner != nullptr ? owner : ::GetDesktopWindow()) /
                static_cast<float>(USER_DEFAULT_SCREEN_DPI);
  // v2.1 诊断：进程工作目录（归一化误判的前提验证，与 config_dir 对比）
  {
    wchar_t cwd_buf[MAX_PATH] = {};
    ::GetCurrentDirectoryW(MAX_PATH, cwd_buf);
    ::ime::LogF("[skin] 设置面板打开：cwd=%ls config_dir=%s\n", cwd_buf,
                config_dir.c_str());
  }
  SettingsContext ctx;
  ctx.draft = skin::DefaultTheme();
  // 已存在 theme.yaml 则以现有配置为编辑起点（面板改的是用户当前主题）
  const auto yaml_path = std::filesystem::path(config_dir) / "theme.yaml";
  if (std::filesystem::exists(yaml_path)) {
    skin::Theme current;
    if (skin::LoadThemeFromYaml(yaml_path.string(), &current)) {
      ctx.draft = current;
      // 旧配置可能存了绝对路径（早前上传逻辑直接存源路径）——若文件就在
      // config 目录内，归一化为相对 config_dir 的路径，保证跨机器迁移可用。
      // v2.1 根因修复：旧实现用 equivalent(candidate,p) 判断，当进程工作目录
      // 恰好 = config 目录时，带子目录的相对名 "skins\frame_x.png" 与
      // candidate 等效 → 被误判为绝对路径 → filename() 只留文件名、丢
      // "skins\" 前缀 → 重开设置窗预览加载失败（align_has_img=false）→
      // 拖拽失效，用户被迫每次重传图片。修复：只对绝对路径归一化，
      // 相对名（含子目录）原样保留；绝对路径用 fs::relative 保前缀。
      auto normalize_skin_path = [&config_dir](std::string& p) {
        if (p.empty()) {
          return;
        }
        std::error_code ec;
        const std::filesystem::path pp(p);
        if (!pp.is_absolute()) {
          return;  // 相对名直接可用（含 skins\ 子目录），不归一化
        }
        const auto candidate =
            std::filesystem::path(config_dir) / pp.filename();
        if (std::filesystem::equivalent(candidate, p, ec)) {
          const auto rel = std::filesystem::relative(p, config_dir, ec);
          if (!rel.empty()) {
            p = rel.string();  // 保留可能的子目录（skins\frame_x.png）
            ::ime::LogF( "[skin] 设置面板：皮肤图归一化为相对路径 → %s\n", p.c_str());
          }
        }
      };
      normalize_skin_path(ctx.draft.skin.role_image);
      normalize_skin_path(ctx.draft.skin.frame_image);
      // 源头归一：旧配置可能存了 "@Aa厚底黑" 这类竖排字体名（@ 前缀 = 旋转 90°
      // 的竖排变体）。面板显示、theme.yaml 落盘、候选窗渲染都必须用常规字体名，
      // 否则字体标签显示乱码、候选窗字体旋转（真机根因 2026-10-03 连续复现）。
      if (!ctx.draft.font.family.empty() && ctx.draft.font.family[0] == '@') {
        ctx.draft.font.family = ctx.draft.font.family.substr(1);
        ::ime::LogF( "[skin] 设置面板：旧配置字体名去 @ 前缀 → %s\n",
                    ctx.draft.font.family.c_str());
      }
    }
  }
  ctx.on_apply = std::move(on_apply);
  ctx.config_dir = config_dir;

  const HINSTANCE hinst = GetModuleHandleW(nullptr);
  static const wchar_t kSettingsClass[] = L"RimeBeautifySettingsWindow";
  {
    WNDCLASSW wc{};
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = hinst;
    // 任务栏/Alt+Tab 图标 = 蝴蝶 logo（资源来自 rime_server 编译的 resource.rc，
    // 对齐官方 WeaselServer 编译 WeaselServer.rc 的做法）
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCE(IDI_APP_LOGO));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // 2026-10-24 DPI 过渡白闪根治：类背景置空，不再用
                                 // COLOR_WINDOW 白刷；擦背景由 WM_ERASEBKGND 填深色底，
                                 // 过渡间隙永远露深色而非白底
    wc.lpszClassName = kSettingsClass;
    RegisterClassW(&wc);
  }

  // 模态对话框：owner 禁用 + 独立消息循环（面板打开期间键盘钩子由调用方放行）。
  // 高度自适应（真机根因 2026-10-03：「底部按钮点不到/ui 窗口没适配」）：
  // 固定 680 高在低分屏/高 DPI 缩放下超出屏幕工作区，底部按钮被截。
  // 这里按主显示器工作区钳制高度；按钮带从底部往上排（下方动态计算），
  // 保证「保存并应用」始终可见可点。
  const int px = DS(80), py = DS(40);
  // 客户区高度（内容/按钮都在客户区坐标内）按主显示器工作区钳制；
  // 创建窗口用 AdjustWindowRect 把「客户区」换算成「外框」（含标题栏+边框）——
  // 这是标准 Win32 对话框做法（同源小狼毫 Weasel 亦按内容尺寸定窗口）。
  // 真机根因 2026-10-04：此前直接拿 work_h 当外框高度，客户区被标题栏吃掉，
  // 底部按钮带（btn_y = work_h - 44）整行落在客户区外 → 截图「保存按键消失」。
  int client_h = DS(kDlgH);
  {
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
      // work 是物理像素；DS 后 kDlgH/边距也是物理，可比
      const int avail = work.bottom - work.top - DS(40) - 24 - DS(48);  // 顶边距+底边距+标题栏/边框
      if (avail < DS(kDlgH)) {
        client_h = avail > DS(560) ? avail : DS(560);  // 极小屏也至少保留 560（卡片区可读）
      }
    }
  }
  RECT frame{0, 0, DS(kDlgW), client_h};
  AdjustWindowRect(&frame, kSettingsStyle, FALSE);  // 客户区 → 外框（标准对话框做法）
  // 2026-10-23：不再 WS_EX_TOPMOST 永久置顶（用户反馈设置窗口不应一直悬浮）。
  // "弹出置顶一下"改在 ShowWindow 前 SetWindowPos(TOPMOST) + 显示后 NOTOPMOST 解除。
  HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, kSettingsClass,
                             ime::i18n::TrW(ime::i18n::Str::kSettingsSkinTitle),
                             kSettingsStyle, px, py,
                             frame.right - frame.left, frame.bottom - frame.top, owner, nullptr,
                             hinst, nullptr);
  if (dlg == nullptr) {
    return false;
  }
  ctx.dlg = dlg;
  SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ctx));

  // 关键修复（2026-10-24 跨屏乱）：创建后立即按窗口所在屏 DPI 重算 scale 并
  // 对齐窗口物理尺寸。此前 g_dpi_scale 用 GetDesktopWindow()（主屏）快照，若
  // 窗口实际创建/显示在更高 DPI 的副屏，窗口按 1x 建小，而 RelayoutControls/
  // PaintPanel 现算 GetDpiForWindow(dlg)=高 DPI → 内容放大溢出小窗口 → 错乱
  //（取色窗因 owner 同 DPI 正常，父级则乱）。强制窗口尺寸 = 所在屏 DPI 的标准
  // 逻辑尺寸，与内容缩放同源，两者彻底一致。
  g_dpi_scale = GetDpiForWindow(dlg) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
  client_h = DS(kDlgH);
  {
    HMONITOR hm = MonitorFromWindow(dlg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hm, &mi)) {
      const int avail = mi.rcWork.bottom - mi.rcWork.top - DS(40) - 24 - DS(48);
      if (avail < DS(kDlgH)) {
        client_h = avail > DS(560) ? avail : DS(560);
      }
    }
  }
  RECT frame2{0, 0, DS(kDlgW), client_h};
  AdjustWindowRect(&frame2, kSettingsStyle, FALSE);
  MoveWindow(dlg, px, py, frame2.right - frame2.left, frame2.bottom - frame2.top, FALSE);

  // 首帧防闪（2026-10-22，用户确认启用 WM_SETREDRAW 标准方案）：窗口创建后
  // 立即冻结重绘（WM_SETREDRAW FALSE），期间创建的 20 余子控件全部不绘制；
  // 全部创建 + 布局 + 内容刷新完成后解冻（TRUE）+ 整窗失效 + ShowWindow，
  // 由系统一次性批量重绘整窗（对话框模板显示语义，微软文档标准防闪 API）。
  // 此前 §32 的 RedrawWindow 先画后显示对不可见窗口无效（系统不绘制不可见
  // 窗口），仅剩"黑白块一闪而过"；用户 2026-10-22 明确要求彻底消除，重新启用
  // §33 记录过的 WM_SETREDRAW 方案（当时因"越弄越复杂"被叫停冻结，现按用户
  // 明确指令恢复实施，m1 §33.2 防再犯同步更新）。
  ::SendMessageW(dlg, WM_SETREDRAW, FALSE, 0);

  // ---- 控件布局（坐标与 PaintPanel 卡片对齐） ----
  // 卡片0 配置管理（y=16 h=84）：下拉 + 加载/删除（行1）、配置名 + 保存当前配置（行2）
  const int c0y = 16, c0h = 84;
  {
    // 下拉列表（CBS_DROPDOWNLIST：点开弹出选项，默认收起只显示选中项）
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                     DS(16), DS(c0y + 32), DS(160), DS(200), dlg,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComboPreset)), hinst, nullptr);
    // v4.4：子类化 COMBOBOX 深色化（系统主题白底直角突兀；见 ComboBoxProc）
    if (combo != nullptr) {
      const auto orig_combo = reinterpret_cast<WNDPROC>(
          SetWindowLongPtrW(combo, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ComboBoxProc)));
      SetPropW(combo, kOrigComboProcProp, reinterpret_cast<HANDLE>(orig_combo));
      // 禁用 comctl32 v6 主题——否则主题在 WM_PAINT 里直接画白底，
      // 我们拦截的 WM_ERASEBKGND 深色底会被覆盖（真机反馈下拉框仍白底）。
      // 动态加载 uxtheme.dll 避免改 CMake 链接。
      static HMODULE h_theme = LoadLibraryW(L"uxtheme.dll");
      if (h_theme != nullptr) {
        const auto p_set_theme = reinterpret_cast<HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR)>(
            GetProcAddress(h_theme, "SetWindowTheme"));
        if (p_set_theme != nullptr) {
          p_set_theme(combo, L"", L"");
        }
      }
    }
  }
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnLoad), kBtnPresetLoad,
                  DS(16 + 172), DS(c0y + 28), DS(72), DS(28));
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnDelete), kBtnPresetDelete,
                  DS(16 + 252), DS(c0y + 28), DS(72), DS(28));
  CreateEdit(dlg, kEditPresetName, DS(16), DS(c0y + 58), DS(160), false);  // v4.3：配置名可输中文/字母
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnSavePreset), kBtnPresetSave,
                  DS(16 + 172), DS(c0y + 54), DS(120), DS(28));
  // 卡片1 颜色（y=114 h=156）：色块（自绘）在左，色值文字由 PaintPanel 画在色块右侧。
  // 色块 26×26 = 设计稿 v3（.color-swatch 26×26）
  // v4.3：垂直间距动态（CardGapForHeight），创建段与 PaintPanel/Relayout 同一公式。
  const int c1y = c0y + c0h + CardGapForHeight(kDlgH);
  CreateSwatch(dlg, kSwatchBg, 14 + 16, c1y + 46, 26, 26);
  CreateSwatch(dlg, kSwatchText, 14 + 222, c1y + 46, 26, 26);
  CreateSwatch(dlg, kSwatchHlText, 14 + 16, c1y + 92, 26, 26);
  CreateSwatch(dlg, kSwatchHlBg, 14 + 222, c1y + 92, 26, 26);
  // 卡片2 字体：字体名由 PaintPanel 自绘（修「字体白条」），按钮在右
  const int c2y = c1y + 156 + CardGapForHeight(kDlgH);
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnPickFont), kBtnFont,
                  330, c2y + 18, 96, 28);
  // 卡片3 皮肤模式 h=272（v2.1 布局卡移除后加高，元素垂直排开）：单选行 y+46
  // （等宽两块 = 设计稿 .mode-option flex:1 平分）、上传按钮 y+90、
  // 文字区域对齐编辑框 y+220（输入框模式显示）
  const int c3y = c2y + 66 + CardGapForHeight(kDlgH);
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kRadioRoleImage), kRadioRole,
                  28, c3y + 46, 197, 28);
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kRadioFrameImage), kRadioFrame,
                  28 + 197 + 10, c3y + 46, 197, 28);
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnPickImage), kBtnRoleImage,
                  230, c3y + 84, 96, 28);
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnPickImage), kBtnFrameImage,
                  230, c3y + 84, 96, 28);
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnOpenFolder), kBtnOpenFolder,
                  330, c3y + 84, 96, 28);  // v4.5.3
  // 文字区域对齐数值微调（X / Y / 宽 / 高，一行 4 个；label 由 PaintPanel 画）
  CreateEdit(dlg, kEditTrX, 16, c3y + 220, 44);
  CreateEdit(dlg, kEditTrY, 16 + 90, c3y + 220, 44);
  CreateEdit(dlg, kEditTrW, 16 + 180, c3y + 220, 44);
  CreateEdit(dlg, kEditTrH, 16 + 270, c3y + 220, 44);
  // v2.1：布局卡（圆角/边距X/间距/行高）整体移除——数值随 theme.yaml，不再提供 UI 自定义。
  // 底部按钮带（y 从窗口底往上算，窗口高度自适应后始终在可视区内）。
  const int btn_y = client_h - 44;
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnReset), kBtnReset,
                  16, btn_y, 100, 30);
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnSaveApply), kBtnSave,
                  160, btn_y, 130, 30);
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnClose), kBtnClose,
                  330, btn_y, 100, 30);

  RefreshAllControls(&ctx);
  RefreshPresetCombo(&ctx);  // v3：配置管理下拉初始化
  RelayoutControls(&ctx, GetDpiForWindow(dlg) / static_cast<float>(USER_DEFAULT_SCREEN_DPI));  // v3.6：宽度自适应坐标单一来源（初始=写死值）

  // 模态消息循环（IsDialogMessage 支持 Tab/方向键在控件间导航）
  if (owner != nullptr) {
    EnableWindow(owner, FALSE);
  }
  // 首帧防闪（2026-10-22 WM_SETREDRAW 解冻 + 整窗失效 + ShowWindow）：
  // 解冻后 RedrawWindow 使整窗与全部子控件失效，ShowWindow 触发系统一轮
  // 完整批量重绘（一次性画完，无"元素逐个冒出"）。窗口创建后的冻结注释见
  // SetWindowLongPtrW(GWLP_USERDATA) 之后。
  ::SendMessageW(dlg, WM_SETREDRAW, TRUE, 0);
  ::RedrawWindow(dlg, nullptr, nullptr,
                 RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME | RDW_INVALIDATE);
  // 弹出瞬间置顶一下，随后解除永久置顶（2026-10-23 用户反馈：设置窗口
  // 不应一直悬浮在最前；原 WS_EX_TOPMOST 已移除）。TOPMOST→Show→前台→NOTOPMOST，
  // 保证弹出时可见可聚焦，之后可被其他窗口自然覆盖。
  ::SetWindowPos(dlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  ::ShowWindow(dlg, SW_SHOW);
  // 打开即前台（修复「打开后需要手动点击才能显示」）
  SetForegroundWindow(dlg);
  ::SetWindowPos(dlg, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    // v2.1 退格修复：IsDialogMessage 对 VK_BACK 的导航判定不稳定（焦点在
    // EDIT 时可能把它当对话框取消/导航键吞掉或转换），退格必须确定性直达
    // EDIT 子类过程（RoundedEditProc 里做 EM 删除）。Tab/方向键/回车仍走
    // IsDialogMessage 导航。
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_BACK) {
      HWND focus = GetFocus();
      if (focus != nullptr) {
        wchar_t fcls[64] = {};
        GetClassNameW(focus, fcls, 64);
        ::ime::LogF("[settings] 主循环退格：focus=0x%p class='%ls'\n", focus, fcls);
        // Windows 返回类名 "Edit"（首字母大写），比较必须不区分大小写
        if (_wcsicmp(fcls, L"EDIT") == 0) {
          ::ime::LogF("[settings] 退格直发 EDIT 子类过程\n");
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
          if (!IsWindow(dlg)) {
            break;
          }
          continue;
        }
        ::ime::LogF("[settings] 退格焦点非 EDIT（%ls），走 IsDialogMessage\n", fcls);
      }
    }
    if (IsDialogMessageW(dlg, &msg)) {
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
    if (!IsWindow(dlg)) {
      break;
    }
  }
  if (msg.message == WM_QUIT) {
    PostQuitMessage(static_cast<int>(msg.wParam));  // 重投，保主循环继续
  }
  if (owner != nullptr) {
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
  }
  return ctx.applied;
}

// ======================================================================
// 自定义颜色选择器（2026-10-22，问题3）：替换系统 CHOOSECOLORW——系统颜色
// 对话框无「吸取屏幕颜色」能力（用户需求）。简洁版：RGB 编辑 + 预览色块 +
// 常用色板 + 吸管取色。吸管 = 官方 Win32 方案：SetCapture 捕获鼠标，移动时
// GetCursorPos + GetPixel(GetDC(NULL)) 实时预览，左键确认取色、Esc/右键取消。
// 风格与主设置窗口一致（深浅色跟随 g_pal、自绘圆角、WM_SETREDRAW 防闪）。
// 注：与 PickColor 处的前向声明同处 unnamed namespace（同一 TU 内多个
// unnamed namespace 是同一实体），符号匹配 internal linkage。
// ======================================================================
namespace {
// 2026-10-22 v2：取色器升级为 HSV 轮盘滑块版（用户要求"轮盘滑块"，参考
// 系统颜色选择器/PS 的色相环 + 饱和度/明度滑块交互）。客户区 420×330。
constexpr int kCPW = 380;
constexpr int kCPH = 470;

// 颜色选择器控件 ID（独立区间，不与主窗口 kSwatchBg.. 冲突）
enum : int {
  kCPEditHex = 300,
  kCPEditR,
  kCPEditG,
  kCPEditB,
  kCPBtnPick,
  kCPBtnOk,
  kCPBtnCancel,
  // 2026-10-24 跨屏 DPI 重排：静态标签也补 ID，便于 MoveWindow 随编辑框联动
  kCPLabelHex,
  kCPLabelR,
  kCPLabelG,
  kCPLabelB,
};

// 拖动状态：0=无，1=SV 面板（饱和度/明度），2=色相条
enum : int { kCPDragNone = 0, kCPDragSV = 1, kCPDragHue = 2 };

struct ColorPickerContext {
  HWND dlg = nullptr;
  std::uint32_t current = 0xFF000000;  // 当前色（0xAARRGGBB，alpha 恒 FF）
  std::uint32_t initial = 0xFF000000;  // 进入时旧色快照（预览"旧→新"对比）
  std::uint32_t* target = nullptr;     // 确定时写入调用方
  bool picking = false;                // 吸管取色模式
  bool syncing = false;                // 程序回写编辑框（防 EN_CHANGE 递归）
  bool confirmed = false;              // 确定按钮按下（模态循环返回值依据）
  int hue = 0;                         // 0-360（色相）
  int sat = 255;                       // 0-255（饱和度）
  int val = 255;                       // 0-255（明度）
  int drag = kCPDragNone;              // 当前拖动的控件
  HWND mask = nullptr;                 // 吸管模式全屏遮罩（吃掉全屏鼠标）
  RECT preview_rc{};                   // 预览色块区（客户区，局部重绘用）
  RECT palette_rc[16]{};               // 色板 16 块（客户区，命中测试用）
  RECT sv_rc{};                        // SV 饱和度/明度面板（命中测试）
  RECT huebar_rc{};                    // 垂直色相条（命中测试）
};

// 常用色板（黑/灰阶/彩色系；白块由 swatch_border 描边保证可见）
constexpr std::uint32_t kPickerPalette[16] = {
    0xFF000000, 0xFF404040, 0xFF808080, 0xFFC0C0C0,
    0xFFFFFFFF, 0xFF800000, 0xFFFF0000, 0xFFFF8000,
    0xFF808000, 0xFFFFFF00, 0xFF008000, 0xFF00FF00,
    0xFF008080, 0xFF00FFFF, 0xFF0000FF, 0xFF8000FF,
};

// HSV → RGB（h:0-360, s/v:0-255）→ 0xAARRGGBB
std::uint32_t HsvToRgb(int h, int s, int v) {
  h = ((h % 360) + 360) % 360;
  const int sector = h / 60;
  const int f = (h * 255 / 60) % 255;
  const int p = v * (255 - s) / 255;
  const int q = v * (255 - s * f / 255) / 255;
  // 2026-10-23 修复（真机实测 FF27FF 玫粉根因）：t 公式 s 与 (255-s) 写反——
  // 原 `v * (255 - (255 - s) * f / 255) / 255` 在 s=0 时输出 v*(255-f)/255≠v
  // （左上角/左边缘出玫粉），在 s=255 时恒等于 v（hue 0-59/120-179/240-299
  // 纯色段整段错误：0° 本应纯红却输出黄）。正确公式 t = v*(1 - s*(1-f))。
  const int t = v * (255 - s * (255 - f) / 255) / 255;
  int r = v, g = v, b = v;
  switch (sector) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return 0xFF000000u |
         (static_cast<std::uint32_t>(r & 0xFF) << 16) |
         (static_cast<std::uint32_t>(g & 0xFF) << 8) |
         static_cast<std::uint32_t>(b & 0xFF);
}

// RGB → HSV（h:0-360, s/v:0-255）
void RgbToHsv(std::uint32_t rgb, int& h, int& s, int& v) {
  const int r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
  const int mx = std::max(r, std::max(g, b));
  const int mn = std::min(r, std::min(g, b));
  v = mx;
  const int delta = mx - mn;
  if (delta == 0) {
    h = 0;
    s = 0;
    return;
  }
  s = delta * 255 / mx;
  int deg = 0;
  if (mx == r) {
    deg = (60 * (g - b) / delta + 360) % 360;
  } else if (mx == g) {
    deg = (60 * (b - r) / delta + 120) % 360;
  } else {
    deg = (60 * (r - g) / delta + 240) % 360;
  }
  h = deg;
}

// 编辑框 → current（EN_CHANGE；syncing 期间跳过，避免程序回写自激）。
// 2026-10-22：HEX 编辑优先（输入完整 6 位 hex 时生效）；RGB 改动同步反算
// hue/sat/val → 重绘 SV 面板与色相条。
void ColorPickerReadEdits(HWND dlg, ColorPickerContext* ctx) {
  if (ctx->syncing) {
    return;
  }
  // HEX 编辑框（#RRGGBB 或 RRGGBB）
  wchar_t buf[16] = {};
  GetDlgItemTextW(dlg, kCPEditHex, buf, 16);
  const wchar_t* p = buf;
  while (*p == L'#' || *p == L' ') p++;
  unsigned long hexv = 0;
  if (wcslen(p) == 6 && swscanf_s(p, L"%lx", &hexv) == 1 && hexv <= 0xFFFFFF) {
    ctx->current = 0xFF000000u | static_cast<std::uint32_t>(hexv);
    RgbToHsv(ctx->current, ctx->hue, ctx->sat, ctx->val);
    InvalidateRect(dlg, &ctx->sv_rc, TRUE);
    InvalidateRect(dlg, &ctx->huebar_rc, TRUE);
    InvalidateRect(dlg, &ctx->preview_rc, TRUE);
    return;
  }
  BOOL ok = FALSE;
  const int r = GetDlgItemInt(dlg, kCPEditR, &ok, FALSE);
  if (!ok) return;
  const int g = GetDlgItemInt(dlg, kCPEditG, &ok, FALSE);
  if (!ok) return;
  const int b = GetDlgItemInt(dlg, kCPEditB, &ok, FALSE);
  if (!ok) return;
  const auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
  ctx->current =
      0xFF000000 | (static_cast<std::uint32_t>(clamp(r)) << 16) |
      (static_cast<std::uint32_t>(clamp(g)) << 8) |
      static_cast<std::uint32_t>(clamp(b));
  RgbToHsv(ctx->current, ctx->hue, ctx->sat, ctx->val);
  InvalidateRect(dlg, &ctx->sv_rc, TRUE);
  InvalidateRect(dlg, &ctx->huebar_rc, TRUE);
  InvalidateRect(dlg, &ctx->preview_rc, TRUE);
}

// current → 编辑框（syncing 包裹防递归；HEX 大写无 #）
void ColorPickerSyncEdits(HWND dlg, ColorPickerContext* ctx) {
  ctx->syncing = true;
  SetDlgItemInt(dlg, kCPEditR, (ctx->current >> 16) & 0xFF, FALSE);
  SetDlgItemInt(dlg, kCPEditG, (ctx->current >> 8) & 0xFF, FALSE);
  SetDlgItemInt(dlg, kCPEditB, ctx->current & 0xFF, FALSE);
  wchar_t hexbuf[16] = {};
  swprintf_s(hexbuf, L"%02X%02X%02X", (ctx->current >> 16) & 0xFF,
             (ctx->current >> 8) & 0xFF, ctx->current & 0xFF);
  SetDlgItemTextW(dlg, kCPEditHex, hexbuf);
  ctx->syncing = false;
}

// 把 hue/sat/val 应用到 current 并同步全部显示（SV 面板/色相条/预览/编辑框）
void ColorPickerApplyHsv(HWND hwnd, ColorPickerContext* ctx) {
  ctx->current = HsvToRgb(ctx->hue, ctx->sat, ctx->val);
  ColorPickerSyncEdits(hwnd, ctx);
  InvalidateRect(hwnd, &ctx->sv_rc, TRUE);
  InvalidateRect(hwnd, &ctx->huebar_rc, TRUE);
  InvalidateRect(hwnd, &ctx->preview_rc, TRUE);
}

// 2026-10-24 跨屏 DPI：取色窗子控件重排（同父级 RelayoutControls 思路）。
// WM_DPICHANGED 已把 g_dpi_scale 设为新 DPI，此处用 DS() 把所有子控件（编辑框/
// 按钮/标签）重排到新 scale，与创建段（DS()）参数完全镜像——否则跨屏后自绘内容
// 放大而子控件停在旧 scale，编辑框/按钮与色板错位（重蹈父级卡片0 的覆辙）。
void RelayoutColorPicker(HWND dlg) {
  MoveWindow(GetDlgItem(dlg, kCPLabelHex), DS(14), DS(294), DS(40), DS(16), TRUE);
  MoveWindow(GetDlgItem(dlg, kCPLabelR), DS(118), DS(294), DS(12), DS(16), TRUE);
  MoveWindow(GetDlgItem(dlg, kCPLabelG), DS(192), DS(294), DS(12), DS(16), TRUE);
  MoveWindow(GetDlgItem(dlg, kCPLabelB), DS(266), DS(294), DS(12), DS(16), TRUE);
  MoveWindow(GetDlgItem(dlg, kCPEditHex), DS(14), DS(310), DS(96), DS(24), TRUE);
  MoveWindow(GetDlgItem(dlg, kCPEditR), DS(118), DS(310), DS(66), DS(24), TRUE);
  MoveWindow(GetDlgItem(dlg, kCPEditG), DS(192), DS(310), DS(66), DS(24), TRUE);
  MoveWindow(GetDlgItem(dlg, kCPEditB), DS(266), DS(310), DS(66), DS(24), TRUE);
  MoveWindow(GetDlgItem(dlg, kCPBtnPick), DS(14), DS(412), DS(138), DS(32), TRUE);
  MoveWindow(GetDlgItem(dlg, kCPBtnOk), DS(160), DS(412), DS(92), DS(32), TRUE);
  MoveWindow(GetDlgItem(dlg, kCPBtnCancel), DS(260), DS(412), DS(92), DS(32), TRUE);
}

void PaintColorPicker(HWND hwnd, ColorPickerContext* ctx) {
  PAINTSTRUCT ps{};
  HDC hdc = BeginPaint(hwnd, &ps);
  // DPI：每窗口绘制时现算 scale
  g_dpi_scale = GetDpiForWindow(hwnd) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
  {
    RECT cr{};
    GetClientRect(hwnd, &cr);
    const int cw = cr.right - cr.left;
    const int ch = cr.bottom - cr.top;
    // 2026-10-22 拖动闪烁根因修复：内存位图双缓冲——此前 GDI+ 直接画到
    // 窗口 DC（单缓冲），每帧"先铺背景再画内容"的过程在屏幕上可见，快速
    // 拖动时人眼看到色盘闪动/白屏（用户实测截图：输入框已更新、色盘/预览
    // 仍是旧帧）。改为先在内存画布完整绘制整帧，再一次性 DrawImage 上屏，
    // 屏幕只见最终帧。
    Gdiplus::Bitmap mem(cw, ch, PixelFormat32bppARGB);
    {
      Gdiplus::Graphics g(&mem);
      g.SetSmoothingMode(SmoothingModeAntiAlias);
      g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
      // 面板底：按客户区实际尺寸铺满（GetClientRect 实测——修复前写死
      // kCPW×kCPH，若客户区因 DPI/边框计算差异大于该值，底部未绘制区域会
      // 透出下层主设置窗口，用户实拍"画面糊在一起"）
      FillRoundRect(g, 0, 0, cw, ch, 0, g_pal->bg_top);

    // ---- 布局常量（与 RunColorPicker 控件坐标一致）----
    const int svx = 14, svy = 14, svw = 216, svh = 216;   // SV 面板
    const int hbx = 248, hby = 14, hbw = 22, hbh = 216;   // 垂直色相条
    const int ppx = 14, ppy = 244, ppw = 38, pph = 38;    // 预览色块（旧/新）
    const int px0 = 14, py0 = 350, pw = 38, ph = 24, gap = 6;  // 色板 2×8

    ctx->sv_rc = {DS(svx), DS(svy), DS(svx + svw), DS(svy + svh)};
    ctx->huebar_rc = {DS(hbx), DS(hby), DS(hbx + hbw), DS(hby + hbh)};

    // ---- SV 面板：逐像素 HSV 生成（S 横轴 / V 纵轴），圆角外保留底色 ----
    // 2026-10-23 修复：原双渐变（gx 白→纯色 + gy 全零透明→黑）的 gy 渐变起点
    // 约 10 行产生 alpha=0 的 ARGB 全零透明像素（_dbg_tools\colorpicker_sv_probe
    // 像素级实证 y=15/17/20 行 R=00 A=00），上屏 DrawImage 默认 SourceOver 混合时
    // 透明像素不覆盖窗口 DC 旧帧 → 色板顶部永远残留打开取色器时的旧色相行
    // （用户真机截图：拖到红色顶部仍是黄色）。改为逐像素 LockBits 写 32bppARGB：
    // 每像素 s=x*255/svw、v=255-y*255/svh 经 HsvToRgb 直接计算（基准 0.31ms，
    // 比原双渐变 0.44ms 更快），四角圆角外跳过保留底色，无任何透明像素。
    {
      Gdiplus::Rect prc(DS(svx), DS(svy), DS(svw), DS(svh));
      Gdiplus::BitmapData pd;
      if (mem.LockBits(&prc, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB,
                       &pd) == Gdiplus::Ok) {
        const int stride = pd.Stride;
        const int sww = DS(svw), swh2 = DS(svh), sr = DS(8);
        BYTE* base = static_cast<BYTE*>(pd.Scan0);
        for (int y = 0; y < swh2; y++) {
          const int v = 255 - y * 255 / swh2;
          for (int x = 0; x < sww; x++) {
            // 圆角 r=8 四角外像素跳过（保持面板底色 bg_top）
            const int ccx = x < sr ? sr : (x >= sww - sr ? sww - sr : -1);
            const int ccy = y < sr ? sr : (y >= swh2 - sr ? swh2 - sr : -1);
            if (ccx >= 0 && ccy >= 0) {
              const int dx = x - ccx, dy = y - ccy;
              if (dx * dx + dy * dy > sr * sr) continue;
            }
            const int s = x * 255 / sww;
            const std::uint32_t c = HsvToRgb(ctx->hue, s, v);
            BYTE* p = base + y * stride + x * 4;
            p[0] = static_cast<BYTE>(c & 0xFF);
            p[1] = static_cast<BYTE>((c >> 8) & 0xFF);
            p[2] = static_cast<BYTE>((c >> 16) & 0xFF);
            p[3] = 0xFF;
          }
        }
        mem.UnlockBits(&pd);
      }
      DrawRoundRect(g, svx, svy, svw, svh, 8, g_pal->card_border);
    }
    // SV 指示点（白圆点 + 深描边）——直接用 GDI+，坐标须 DS（否则跨 DPI 错位）
    {
      const int dotx = DS(svx + ctx->sat * svw / 255);
      const int doty = DS(svy + (255 - ctx->val) * svh / 255);
      SolidBrush hb{Color(0xFFFFFFFF)};
      g.FillEllipse(&hb, dotx - DS(7), doty - DS(7), DS(14), DS(14));
      Pen hp{Color(0xFF2A2A2A), 1.5f * g_dpi_scale};
      g.DrawEllipse(&hp, dotx - DS(7), doty - DS(7), DS(14), DS(14));
    }

    // ---- 垂直色相条：彩虹渐变（红→黄→绿→青→蓝→品红→红），圆角裁剪 ----
    {
      GraphicsPath clip;
      const float r = DS(8.0f);
      const float hbx2 = static_cast<float>(DS(hbx)), hby2 = static_cast<float>(DS(hby));
      const float hbw2 = static_cast<float>(DS(hbw)), hbh2 = static_cast<float>(DS(hbh));
      clip.AddArc(hbx2, hby2, 2 * r, 2 * r, 180, 90);
      clip.AddArc(hbx2 + hbw2 - 2 * r, hby2, 2 * r, 2 * r, 270, 90);
      clip.AddArc(hbx2 + hbw2 - 2 * r, hby2 + hbh2 - 2 * r, 2 * r, 2 * r, 0, 90);
      clip.AddArc(hbx2, hby2 + hbh2 - 2 * r, 2 * r, 2 * r, 90, 90);
      clip.CloseFigure();
      g.SetClip(&clip);
      Color hcolors[7] = {Color(0xFFFF0000), Color(0xFFFFFF00), Color(0xFF00FF00),
                          Color(0xFF00FFFF), Color(0xFF0000FF), Color(0xFFFF00FF),
                          Color(0xFFFF0000)};
      float hpos[7] = {0.0f, 1.0f / 6.0f, 2.0f / 6.0f, 3.0f / 6.0f, 4.0f / 6.0f,
                       5.0f / 6.0f, 1.0f};
      LinearGradientBrush gh(RectF(hbx2, hby2, hbw2, hbh2),
                             hcolors[0], hcolors[6], Gdiplus::LinearGradientModeVertical);
      gh.SetInterpolationColors(hcolors, hpos, 7);
      g.FillRectangle(&gh, hbx2, hby2, hbw2, hbh2);
      g.ResetClip();
      DrawRoundRect(g, hbx, hby, hbw, hbh, 8, g_pal->card_border);
    }
    // 色相条指示（白色圆角横条）
    {
      const int knoby = hby + ctx->hue * hbh / 360;
      FillRoundRect(g, hbx - 3, knoby - 4, hbw + 6, 8, 4, 0xFFFFFFFF);
      DrawRoundRect(g, hbx - 3, knoby - 4, hbw + 6, 8, 4, 0xFF2A2A2A);
    }

    // ---- 预览：旧色（进入快照）→ 新色（当前），右侧 HEX + RGB 副文字 ----
    // 预览重绘区必须覆盖右侧 HEX/RGB 文字（文字 x=ppx+2*ppw+20 起）。此前
    // 右边界只到 ppx+2*ppw+10，文字落在重绘范围外→色板/吸管变化时文字不刷新、
    // 永远显示进入时的值，确定后整窗重绘才变（用户截图 #292929 不变铁证）。
    ctx->preview_rc = {DS(ppx), DS(ppy), DS(ppx + 2 * ppw + 10 + 160), DS(ppy + pph)};
    FillRoundRect(g, ppx, ppy, ppw, pph, 8, ctx->initial);
    DrawRoundRect(g, ppx, ppy, ppw, pph, 8, g_pal->card_border);
    FillRoundRect(g, ppx + ppw + 10, ppy, ppw, pph, 8, ctx->current);
    DrawRoundRect(g, ppx + ppw + 10, ppy, ppw, pph, 8, g_pal->card_border);
    {
      wchar_t hex[16] = {};
      swprintf_s(hex, L"#%02X%02X%02X", (ctx->current >> 16) & 0xFF,
                 (ctx->current >> 8) & 0xFF, ctx->current & 0xFF);
      DrawTextC(g, hex, L"Consolas", 17, ppx + 2 * ppw + 20, ppy, g_pal->text);
      wchar_t rgb[32] = {};
      swprintf_s(rgb, L"R %d · G %d · B %d", (ctx->current >> 16) & 0xFF,
                 (ctx->current >> 8) & 0xFF, ctx->current & 0xFF);
      DrawTextC(g, rgb, L"Consolas", 11, ppx + 2 * ppw + 20, ppy + 24,
                g_pal->text_sub);
    }

    // ---- 色板 2 行 x 8 列（38×24，圆角 6）----
    for (int i = 0; i < 16; i++) {
      const int qx = px0 + (i % 8) * (pw + gap);
      const int qy = py0 + (i / 8) * (ph + gap);
      ctx->palette_rc[i] = {DS(qx), DS(qy), DS(qx + pw), DS(qy + ph)};
      FillRoundRect(g, qx, qy, pw, ph, 6, kPickerPalette[i]);
      DrawRoundRect(g, qx, qy, pw, ph, 6, g_pal->swatch_border);
    }

    // ---- 吸管取色提示（仅 picking 时显示，底部一行）----
    if (ctx->picking) {
      DrawTextC(g, ime::i18n::TrW(ime::i18n::Str::kPickingHint),
                L"Microsoft YaHei UI", 11, 14, ch - 22, g_pal->text_sub);
    }
    }
    // 内存整帧 → 一次性上屏（双缓冲，避免单缓冲重绘过程可见的闪烁）
    Gdiplus::Graphics win(hdc);
    win.SetSmoothingMode(SmoothingModeAntiAlias);
    win.DrawImage(&mem, 0, 0, cw, ch);
  }
  EndPaint(hwnd, &ps);
}

// 按钮自绘（圆角 + 文字；确定=主按钮配色，其余=次要按钮配色，
// 与主设置窗口 PaintOwnButton 同风格、同调色板 g_pal 深浅自适应）
void PaintPickerButton(LPDRAWITEMSTRUCT dis) {
  g_dpi_scale = GetDpiForWindow(dis->hwndItem) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
  const bool down = (dis->itemState & ODS_SELECTED) != 0;
  const bool hot = (dis->itemState & ODS_HOTLIGHT) != 0;
  const bool primary = (dis->CtlID == kCPBtnOk);
  const int x = dis->rcItem.left, y = dis->rcItem.top;
  const int w = dis->rcItem.right - x, h = dis->rcItem.bottom - y;
  wchar_t text[64] = {};
  GetWindowTextW(dis->hwndItem, text, 64);
  std::uint32_t fill = primary ? g_pal->btn_primary : g_pal->btn_secondary;
  if (down) fill = primary ? g_pal->btn_primary_down : g_pal->btn_secondary_down;
  else if (hot) fill = primary ? g_pal->btn_primary_hover : g_pal->btn_secondary_hover;
  Bitmap bmp(w, h, PixelFormat32bppARGB);
  {
    Graphics bg(&bmp);
    bg.SetSmoothingMode(SmoothingModeAntiAlias);
    bg.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    bg.Clear(Color(0, 0, 0, 0));  // 透明底（圆角外透出面板底色）
    FillRoundRect(bg, 0, 0, w, h, 7, fill);
    if (!primary) {
      DrawRoundRect(bg, 0, 0, w, h, 7, g_pal->btn_secondary_border);
    }
    Font font(L"Microsoft YaHei UI", 13.0f * g_dpi_scale, FontStyleRegular, UnitPixel);
    SolidBrush br{Color(primary ? g_pal->btn_primary_text : g_pal->btn_secondary_text)};
    StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    bg.DrawString(text, -1, &font,
                  RectF(0, 0, static_cast<float>(w), static_cast<float>(h)),
                  &fmt, &br);
  }
  Graphics g(dis->hDC);
  g.SetSmoothingMode(SmoothingModeAntiAlias);
  g.DrawImage(&bmp, x, y, w, h);
}

// 吸管模式结束：销毁全屏遮罩 + 取色窗口恢复显示/聚焦/刷新提示
void EndPick(ColorPickerContext* ctx) {
  if (ctx->mask != nullptr && IsWindow(ctx->mask)) {
    DestroyWindow(ctx->mask);
  }
  ctx->mask = nullptr;
  ctx->picking = false;
  // 防御：确保十字光标立即恢复正常（遮罩销毁后系统会重设，这里显式恢复，
  // 防止"叉掉窗口后鼠标永远是十字"卡死）
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  if (ctx->dlg != nullptr && IsWindow(ctx->dlg)) {
    // 吸管期间取色窗被临时置顶（盖在遮罩之上）；吸管结束恢复显示时解除
    // 永久置顶（用户要求：取色窗仅在刚打开/取色时置顶，不一直悬浮最前）。
    ::SetWindowPos(ctx->dlg, HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE);
    ShowWindow(ctx->dlg, SW_SHOW);
    SetForegroundWindow(ctx->dlg);
    InvalidateRect(ctx->dlg, nullptr, TRUE);  // 刷新提示文字
  }
}

// 全屏取色遮罩窗口（吸管模式；对齐 PowerToys/Krypton 等标准实现：全屏遮罩
// 吃掉所有鼠标事件、十字光标全局保持、点击遮罩任意处即取色，绝不点穿到
// 下层网页。取色窗口重新置顶显示在遮罩之上，保持可见实时预览。）
const wchar_t kPickMaskClass[] = L"RimeBeautifyPickMask";

LRESULT CALLBACK PickMaskProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  ColorPickerContext* ctx = reinterpret_cast<ColorPickerContext*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;  // 分层透明遮罩，无背景可擦
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_CROSS));
      return TRUE;
    case WM_MOUSEMOVE:
      if (ctx == nullptr || ctx->dlg == nullptr) {
        break;
      }
      {
        // 实时预览：取屏幕坐标像素 → 更新取色窗口（编辑框/SV 面板/色相条/预览）
        POINT pt{};
        GetCursorPos(&pt);
        const HDC dc = GetDC(nullptr);
        const COLORREF c =
            dc != nullptr ? GetPixel(dc, pt.x, pt.y) : CLR_INVALID;
        if (dc != nullptr) ReleaseDC(nullptr, dc);
        if (c != CLR_INVALID) {
          ctx->current = 0xFF000000 | (GetRValue(c) << 16) |
                         (GetGValue(c) << 8) | GetBValue(c);
          RgbToHsv(ctx->current, ctx->hue, ctx->sat, ctx->val);
          ColorPickerSyncEdits(ctx->dlg, ctx);
          InvalidateRect(ctx->dlg, &ctx->sv_rc, TRUE);
          InvalidateRect(ctx->dlg, &ctx->huebar_rc, TRUE);
          InvalidateRect(ctx->dlg, &ctx->preview_rc, TRUE);
        }
      }
      return 0;
    case WM_LBUTTONUP:
      if (ctx == nullptr || ctx->dlg == nullptr) {
        break;
      }
      {
        // 点击=确认+写回+关闭（对齐 PowerToys/系统取色器：吸管点哪取哪、
        // 立即确定并关闭，用户无需再回取色窗点"确定"）。此前只 EndPick 恢复
        // 取色窗、停在窗口里要二次点确定，与提示词不符。
        POINT pt{};
        GetCursorPos(&pt);
        const HDC dc = GetDC(nullptr);
        const COLORREF c =
            dc != nullptr ? GetPixel(dc, pt.x, pt.y) : CLR_INVALID;
        if (dc != nullptr) ReleaseDC(nullptr, dc);
        if (c != CLR_INVALID) {
          ctx->current = 0xFF000000 | (GetRValue(c) << 16) |
                         (GetGValue(c) << 8) | GetBValue(c);
          RgbToHsv(ctx->current, ctx->hue, ctx->sat, ctx->val);
          ColorPickerSyncEdits(ctx->dlg, ctx);   // 写回取色窗编辑框
          InvalidateRect(ctx->dlg, &ctx->preview_rc, TRUE);
          InvalidateRect(ctx->dlg, &ctx->sv_rc, TRUE);
          InvalidateRect(ctx->dlg, &ctx->huebar_rc, TRUE);
          ::ime::LogF("[settings] 吸管点击取色 0x%08lX（写回预览，不关闭，可继续调整）\n",
                      static_cast<unsigned long>(ctx->current));
        }
        EndPick(ctx);  // 取消吸管、恢复取色窗；用户可继续调整后点"确定"写回
      }
      return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      // 用户要求：去掉右键取消功能——右键在遮罩上会呼出右键菜单，体验复杂。
      // 取消吸管只用 Esc。这里直接吞掉右键，避免穿透/菜单。
      return 0;
    case WM_KEYDOWN:
      // 焦点可能在遮罩上（点击遮罩时激活）——此时取色窗收不到 Esc，必须在
      // 遮罩侧 EndPick 取消吸管回取色窗（不关闭窗口）。
      if (wParam == VK_ESCAPE && ctx != nullptr) {
        ::ime::LogF("[settings] 吸管 Esc 取消(焦点在遮罩)\n");
        EndPick(ctx);
        return 0;
      }
      break;
    case WM_CAPTURECHANGED:
      // 兜底：捕获异常丢失也不遗留遮罩（遮罩本身不 SetCapture，防御性处理）
      if (ctx != nullptr && ctx->picking) {
        EndPick(ctx);
      }
      break;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ColorPickerProc(HWND hwnd, UINT msg, WPARAM wParam,
                                 LPARAM lParam) {
  ColorPickerContext* ctx = reinterpret_cast<ColorPickerContext*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      const int code = HIWORD(wParam);
      if (code == EN_CHANGE &&
          (id == kCPEditHex || id == kCPEditR || id == kCPEditG ||
           id == kCPEditB)) {
        if (ctx != nullptr) ColorPickerReadEdits(hwnd, ctx);
        return 0;
      }
      if (code == BN_CLICKED) {
        if (id == kCPBtnPick) {
          // 进入吸管取色：创建全屏透明遮罩（对齐 PowerToys/系统取色器标准
          // 实现）——遮罩吃掉全屏鼠标事件、十字光标全局保持、点击遮罩任意
          // 处即取色，不会点穿到下层网页；取色窗口重新置顶保持可见实时预览。
          if (ctx != nullptr && !ctx->picking) {
            static bool mask_registered = false;
            if (!mask_registered) {
              WNDCLASSW wc{};
              wc.lpfnWndProc = PickMaskProc;
              wc.hInstance = GetModuleHandleW(nullptr);
              wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
              wc.hbrBackground = nullptr;
              wc.lpszClassName = kPickMaskClass;
              RegisterClassW(&wc);
              mask_registered = true;
            }
            // 遮罩覆盖全部监视器（含副屏/不同布局）：用虚拟屏幕坐标。仅用
            // SM_CXSCREEN/SM_CYSCREEN+原点(0,0)时副屏无遮罩取不到色；副屏在
            // 主屏左侧/上方时 SM_XVIRTUALSCREEN 为负。对齐 PowerToys 全屏取色。
            const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            HWND mask = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                kPickMaskClass, L"", WS_POPUP, vx, vy, vw, vh, nullptr, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            if (mask != nullptr) {
              SetLayeredWindowAttributes(mask, 0, 1, LWA_ALPHA);  // 全透明，仅吃鼠标
              SetWindowLongPtrW(mask, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(ctx));
              ctx->mask = mask;
              ctx->picking = true;
              ShowWindow(mask, SW_SHOW);
              // 取色窗口重新置顶，显示在遮罩之上（保持可见、可点确定/取消）
              SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
              SetForegroundWindow(hwnd);
              InvalidateRect(hwnd, nullptr, TRUE);
            }
          }
          return 0;
        }
        if (id == kCPBtnOk) {
          if (ctx != nullptr) {
            // 吸管模式中直接确定：先销毁遮罩再写回关闭
            if (ctx->picking && ctx->mask != nullptr && IsWindow(ctx->mask)) {
              DestroyWindow(ctx->mask);
            }
            ctx->mask = nullptr;
            ctx->picking = false;
            ctx->confirmed = true;
            // 2026-10-22 修复：此前只置 confirmed 未写回 *target——PickColor
            // 读到的仍是旧值，用户"改色点确定外面不变色"（真机实锤）。
            *ctx->target = ctx->current;
            ::ime::LogF("[settings] 取色确定写回 target=0x%08lX\n",
                        static_cast<unsigned long>(ctx->current));
          }
          DestroyWindow(hwnd);
          return 0;
        }
        if (id == kCPBtnCancel) {
          if (ctx != nullptr) {
            if (ctx->picking && ctx->mask != nullptr && IsWindow(ctx->mask)) {
              DestroyWindow(ctx->mask);
            }
            ctx->mask = nullptr;
            ctx->picking = false;
            ::ime::LogF("[settings] 取色取消，未写回（当前=0x%08lX）\n",
                        static_cast<unsigned long>(ctx->current));
          }
          DestroyWindow(hwnd);
          return 0;
        }
        // IsDialogMessage 把 Esc 转成 WM_COMMAND(IDCANCEL)（不派发 WM_KEYDOWN）
        if (id == IDCANCEL) {
          if (ctx != nullptr && ctx->picking) {
            // Esc 取消吸管：EndPick 销毁遮罩、鼠标回正常、回到取色窗。
            // 不关闭窗口——用户明确"按 esc 是取消、变回正常鼠标，别关颜色窗"。
            ::ime::LogF("[settings] 吸管 Esc 取消\n");
            EndPick(ctx);
            return 0;
          }
          DestroyWindow(hwnd);
          return 0;
        }
      }
      break;
    }
    case WM_MOUSEMOVE:
      // 拖动 SV 面板/色相条（capture 期间）优先；吸管取色其次
      if (ctx != nullptr && ctx->drag != kCPDragNone) {
        const int mx = static_cast<short>(LOWORD(lParam));
        const int my = static_cast<short>(HIWORD(lParam));
        if (ctx->drag == kCPDragSV) {
          const int w = ctx->sv_rc.right - ctx->sv_rc.left;
          const int h = ctx->sv_rc.bottom - ctx->sv_rc.top;
          int s = (mx - ctx->sv_rc.left) * 255 / w;
          int v = 255 - (my - ctx->sv_rc.top) * 255 / h;
          ctx->sat = s < 0 ? 0 : (s > 255 ? 255 : s);
          ctx->val = v < 0 ? 0 : (v > 255 ? 255 : v);
          ColorPickerApplyHsv(hwnd, ctx);
        } else if (ctx->drag == kCPDragHue) {
          const int h = ctx->huebar_rc.bottom - ctx->huebar_rc.top;
          int hue = (my - ctx->huebar_rc.top) * 360 / h;
          ctx->hue = hue < 0 ? 0 : (hue > 359 ? 359 : hue);
          ColorPickerApplyHsv(hwnd, ctx);
        }
        return 0;
      }
      break;
    case WM_LBUTTONUP:
      if (ctx != nullptr && ctx->drag != kCPDragNone) {
        ctx->drag = kCPDragNone;
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
      if (ctx != nullptr && !ctx->picking) {
        // 客户区坐标
        const int mx = static_cast<short>(LOWORD(lParam));
        const int my = static_cast<short>(HIWORD(lParam));
        // SV 面板命中（饱和度=横向 X，明度=纵向 Y）
        if (mx >= ctx->sv_rc.left && mx <= ctx->sv_rc.right &&
            my >= ctx->sv_rc.top && my <= ctx->sv_rc.bottom) {
          const int w = ctx->sv_rc.right - ctx->sv_rc.left;
          const int h = ctx->sv_rc.bottom - ctx->sv_rc.top;
          int s = (mx - ctx->sv_rc.left) * 255 / w;
          int v = 255 - (my - ctx->sv_rc.top) * 255 / h;
          ctx->sat = s < 0 ? 0 : (s > 255 ? 255 : s);
          ctx->val = v < 0 ? 0 : (v > 255 ? 255 : v);
          ColorPickerApplyHsv(hwnd, ctx);
          ctx->drag = kCPDragSV;
          SetCapture(hwnd);
          return 0;
        }
        // 垂直色相条命中
        if (mx >= ctx->huebar_rc.left && mx <= ctx->huebar_rc.right &&
            my >= ctx->huebar_rc.top && my <= ctx->huebar_rc.bottom) {
          const int h = ctx->huebar_rc.bottom - ctx->huebar_rc.top;
          int hue = (my - ctx->huebar_rc.top) * 360 / h;
          ctx->hue = hue < 0 ? 0 : (hue > 359 ? 359 : hue);
          ColorPickerApplyHsv(hwnd, ctx);
          ctx->drag = kCPDragHue;
          SetCapture(hwnd);
          return 0;
        }
        // 色板命中
        for (int i = 0; i < 16; i++) {
          if (mx >= ctx->palette_rc[i].left && mx < ctx->palette_rc[i].right &&
              my >= ctx->palette_rc[i].top &&
              my < ctx->palette_rc[i].bottom) {
            ctx->current = kPickerPalette[i];
            RgbToHsv(ctx->current, ctx->hue, ctx->sat, ctx->val);
            ColorPickerSyncEdits(hwnd, ctx);
            InvalidateRect(hwnd, &ctx->sv_rc, TRUE);
            InvalidateRect(hwnd, &ctx->huebar_rc, TRUE);
            InvalidateRect(hwnd, &ctx->preview_rc, TRUE);
            return 0;
          }
        }
      }
      break;
    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) {
        // IsDialogMessage 通常已把 Esc 转 WM_COMMAND(IDCANCEL)；此处兜底
        //（直发消息/捕获期间派发 WM_KEYDOWN 的场景）
        if (ctx != nullptr && ctx->picking) {
          ::ime::LogF("[settings] 吸管 Esc 取消(直发)\n");
          EndPick(ctx);  // 取消吸管回取色窗，不关闭窗口
        } else {
          DestroyWindow(hwnd);
        }
        return 0;
      }
      break;
    case WM_ERASEBKGND:
      // 2026-10-27 回归修复（同父级）：返回 1 禁止擦除，双缓冲整帧覆盖。
      // 2026-10-24 在此 FillRect 裸画底色——SV 色盘/色相条拖动时每帧
      // InvalidateRect(TRUE) 先触发此处裸画再 WM_PAINT，快速拖动露纯底色帧
      // → 色盘闪动（用户回归 10/23 不闪、10/24 后闪）。去掉后跨屏过渡露旧帧
      // （类背景 null 不自动擦白），不闪白。跨屏 DPI 其余逻辑一行不动。
      return 1;
    case WM_PAINT:
      if (ctx != nullptr) {
        PaintColorPicker(hwnd, ctx);
      } else {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
      }
      return 0;
    case WM_DRAWITEM:
      PaintPickerButton(reinterpret_cast<LPDRAWITEMSTRUCT>(lParam));
      return TRUE;
    case WM_CTLCOLORBTN:
      // 按钮圆角外 4 角擦背景用面板底色（2026-10-22 死角修复，见主窗口注释）
      return reinterpret_cast<LRESULT>(GetPanelBrush());
    case WM_CTLCOLOREDIT: {
      const HDC dc = reinterpret_cast<HDC>(wParam);
      SetTextColor(dc, RGB((g_pal->text >> 16) & 0xFF,
                           (g_pal->text >> 8) & 0xFF, g_pal->text & 0xFF));
      SetBkColor(dc, RGB((g_pal->edit_bg >> 16) & 0xFF,
                         (g_pal->edit_bg >> 8) & 0xFF,
                         g_pal->edit_bg & 0xFF));
      return reinterpret_cast<LRESULT>(GetEditBrush());
    }
    case WM_CTLCOLORSTATIC: {
      const HDC dc = reinterpret_cast<HDC>(wParam);
      SetTextColor(dc, RGB((g_pal->text >> 16) & 0xFF,
                           (g_pal->text >> 8) & 0xFF, g_pal->text & 0xFF));
      SetBkMode(dc, TRANSPARENT);
      return reinterpret_cast<LRESULT>(GetCardBrush());
    }
    case WM_CLOSE:
      // 关窗=Esc（2026-10-24 严重 bug 修复）：取色中若直接叉掉窗口，必须先
      // EndPick 销毁全屏遮罩 + 恢复光标，否则遮罩残留、鼠标永远变十字卡死。
      if (ctx != nullptr && ctx->picking) {
        EndPick(ctx);
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DPICHANGED: {
      // MSDN 官方标准（同父级）：用 lParam 建议矩形 SetWindowPos 缩放窗口，
      // 内容 scale 用 wParam 新 DPI（不能用 GetDpiForWindow，跨屏瞬间返回旧值）。
      const UINT new_dpi = LOWORD(static_cast<DWORD>(wParam));
      g_dpi_scale = new_dpi / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
      const RECT* prc = reinterpret_cast<const RECT*>(lParam);
      if (prc != nullptr) {
        // 2026-10-24 DPI 过渡（同父级方案）：SWP_NOCOPYBITS 禁止系统把旧尺寸内容
        // 拉伸贴到新窗口，过渡间隙由 WM_ERASEBKGND 填当前主题底色，再整帧重绘。
        SetWindowPos(hwnd, nullptr, prc->left, prc->top,
                     prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
      }
      // 2026-10-24 跨屏 DPI：重排子控件到新 scale（g_dpi_scale 已用 wParam 新 DPI
      // 设置），与创建段 DS() 镜像，避免自绘放大而编辑框/按钮停在旧 scale。
      RelayoutColorPicker(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }
    case WM_SETTINGCHANGE:
      RefreshPalette();
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 模态取色对话框：owner 禁用 + 独立消息循环（与 RunSettingsDialog 同模式；
// IsDialogMessage 支持 Tab/方向键控件导航，Esc 由 IDCANCEL 分支处理）
bool RunColorPicker(HWND owner, std::uint32_t* target) {
  RefreshPalette();
  // DPI 缩放：取色窗跟随 owner 所在屏 dpi（布局/控件/命中一致放大）
  g_dpi_scale = GetDpiForWindow(owner != nullptr ? owner : ::GetDesktopWindow()) /
                static_cast<float>(USER_DEFAULT_SCREEN_DPI);
  const HINSTANCE hinst = GetModuleHandleW(nullptr);
  static const wchar_t kCPClass[] = L"RimeBeautifyColorPicker";
  static bool cp_registered = false;
  if (!cp_registered) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = ColorPickerProc;
    wc.hInstance = hinst;
    wc.hIcon = LoadIconW(hinst, MAKEINTRESOURCE(IDI_APP_LOGO));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // 2026-10-24 DPI 过渡（同父级方案）：类背景置空，
                                 // 不再用 COLOR_WINDOW 白刷；擦背景由 WM_ERASEBKGND
                                 // 填当前主题底色
    wc.lpszClassName = kCPClass;
    RegisterClassW(&wc);
    cp_registered = true;
  }
  ColorPickerContext ctx;
  ctx.current = (*target & 0x00FFFFFF) | 0xFF000000;  // alpha 恒 FF（主题色）
  ctx.initial = ctx.current;                          // 旧色快照（预览对比）
  ctx.target = target;
  RgbToHsv(ctx.current, ctx.hue, ctx.sat, ctx.val);  // SV 面板/色相条初始值
  // 初始位置：owner 窗口中央
  RECT orc{};
  GetWindowRect(owner, &orc);
  const int ox = orc.left + ((orc.right - orc.left) - kCPW) / 2;
  const int oy = orc.top + ((orc.bottom - orc.top) - kCPH) / 2;
  // 固定大小（无 WS_THICKFRAME）；WS_EX_DLGMODALFRAME + WS_EX_TOPMOST 对齐
  // 主设置窗口。
  // 2026-10-22 修复：客户区 → 外框必须 AdjustWindowRect 换算（标准 Win32
  // 对话框做法，对齐 RunSettingsDialog §30 教训）——修复前把 kCPW×kCPH 直接
  // 当窗口外框尺寸，客户区被标题栏/边框吃掉约 30px，底部按钮（吸管/确定/
  // 取消，y=218..248）被裁掉不可见，且 WM_PAINT 按 kCPW×kCPH 绘制的内容
  // 超出客户区被裁剪、客户区底部未绘制区域透出下层主设置窗口 → 用户实测
  // "画面糊在一起、根本改不了颜色"。
  constexpr DWORD kCPStyle = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
  RECT frame{0, 0, DS(kCPW), DS(kCPH)};
  AdjustWindowRect(&frame, kCPStyle, FALSE);  // 客户区 → 外框（含标题栏/边框）
  HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kCPClass,
                             ime::i18n::TrW(ime::i18n::Str::kColorPickerTitle),
                             kCPStyle, ox, oy,
                             frame.right - frame.left, frame.bottom - frame.top,
                             owner, nullptr, hinst, nullptr);
  if (dlg == nullptr) {
    return false;
  }
  {
    RECT rc{};
    GetClientRect(dlg, &rc);
    const UINT dpi = GetDpiForWindow(dlg);
    ::ime::LogF("[settings] RunColorPicker 创建: 外框=%dx%d 客户区=%dx%d DPI=%u 初始target=0x%08lX\n",
                frame.right - frame.left, frame.bottom - frame.top,
                rc.right - rc.left, rc.bottom - rc.top, dpi,
                static_cast<unsigned long>(*target));
  }
  ctx.dlg = dlg;
  SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ctx));
  // 首帧防闪：冻结重绘 → 创建控件 → 解冻 + 整窗失效 + ShowWindow（与主设置
  // 窗口 2026-10-22 WM_SETREDRAW 方案一致，避免控件逐个绘制露色块）
  ::SendMessageW(dlg, WM_SETREDRAW, FALSE, 0);
  // 输入行（label y=294，编辑框 y=310 高 24；HEX 96 宽，R/G/B 各 66 宽）
  CreateLabel(dlg, L"HEX", DS(14), DS(294), DS(40), DS(16), kCPLabelHex);
  CreateEdit(dlg, kCPEditHex, DS(14), DS(310), DS(96), false);
  CreateLabel(dlg, L"R", DS(118), DS(294), DS(12), DS(16), kCPLabelR);
  CreateEdit(dlg, kCPEditR, DS(118), DS(310), DS(66));
  CreateLabel(dlg, L"G", DS(192), DS(294), DS(12), DS(16), kCPLabelG);
  CreateEdit(dlg, kCPEditG, DS(192), DS(310), DS(66));
  CreateLabel(dlg, L"B", DS(266), DS(294), DS(12), DS(16), kCPLabelB);
  CreateEdit(dlg, kCPEditB, DS(266), DS(310), DS(66));
  // 按钮行（吸取/确定/取消；确定为主按钮配色）
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnPickScreenColor), kCPBtnPick,
                  DS(14), DS(412), DS(138), DS(32));
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnOk), kCPBtnOk,
                  DS(160), DS(412), DS(92), DS(32));
  CreateOwnButton(dlg, ime::i18n::TrW(ime::i18n::Str::kBtnCancel), kCPBtnCancel,
                  DS(260), DS(412), DS(92), DS(32));
  ColorPickerSyncEdits(dlg, &ctx);
  if (owner != nullptr) {
    // 标准模态对话框行为（对齐系统 CHOOSECOLORW）：仅禁用 owner，不隐藏。
    // 2026-10-22 曾试"取色期间隐藏 owner"（因取色窗口盖不住设置窗口、四周
    // 露出设置内容观感混乱），用户指出正常软件不杀下层窗口——回退为仅禁用，
    // 配合新 UI（380×470 精致面板）的独立呈现解决观感问题。
    EnableWindow(owner, FALSE);
  }
  ::SendMessageW(dlg, WM_SETREDRAW, TRUE, 0);
  ::RedrawWindow(dlg, nullptr, nullptr,
                 RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME | RDW_INVALIDATE);
  // 用户要求：颜色取色器窗口「刚打开时置顶（让用户能找到），显示后解除」，
  // 不一直悬浮最前。创建时已带 WS_EX_TOPMOST 保证弹出在最前，这里显示+抢
  // 前台后立即解除永久置顶，之后可被其他窗口自然覆盖（吸管取色时再临时置顶）。
  ::SetWindowPos(dlg, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
  ::ShowWindow(dlg, SW_SHOW);
  SetForegroundWindow(dlg);
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (IsDialogMessageW(dlg, &msg)) {
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
    if (!IsWindow(dlg)) {
      break;
    }
  }
  if (msg.message == WM_QUIT) {
    PostQuitMessage(static_cast<int>(msg.wParam));  // 重投，保主循环继续
  }
  if (owner != nullptr) {
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
  }
  return ctx.confirmed;
}

}  // namespace

}  // namespace ime::ui
