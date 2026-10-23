#include "candidate_window.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// windows.h 的 min/max 宏会破坏 std::min/std::max（NOMINMAX 已在
// candidate_window.h 顶部、include <windows.h> 之前定义；此处再以
// (std::min) 括号形式调用，双保险免疫任何宏展开——C2589 防再犯）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <gdiplus.h>

#include "skin/theme.h"
#include "log/log.h"

// 统一日志文件：rime_server.exe 内为 rime_server.log，rime_tsf.dll 内为
// rime_tsf.log（由各二进制内的 ime::g_log 定义提供；正式版无 stdout）
namespace ime { extern FILE* g_log; }

namespace ime::ui {

namespace {

// GDI+ 一次性启动（PNG 背景渲染需要）。注意：析构**不调用 GdiplusShutdown**
// ——微软官方文档明确禁止在 DllMain（loader lock）内调用 GdiplusShutdown
// （会死锁/挂起）。本 DLL 的全局对象析构正发生在卸载路径（loader lock 下），
// 故关闭动作必须省略；GDI+ 资源由进程退出时系统回收（regsvr32/宿主均为短命
// 进程，无累积泄漏）。候选窗销毁前已释放各自 Bitmap（GetCachedSkinBitmap 的
// delete + CandidateWindow 析构）。
struct GdiplusStartupRAII {
  ULONG_PTR token = 0;
  GdiplusStartupRAII() {
    Gdiplus::GdiplusStartupInput input;
    Gdiplus::GdiplusStartup(&token, &input, nullptr);
  }
  ~GdiplusStartupRAII() {
    // 对齐微软官方：DllMain 内禁止 GdiplusShutdown。故意留空。
  }
};
GdiplusStartupRAII g_gdiplus;

// 实际文字排版区：显式指定（width/height>1）直接用；否则（未设置全 0、
// 或 width/height ≤1——v4.5 相对比例残留与设置窗曾写成的 1×1 塌缩）按
// frame_image 尺寸推导为整个中间区（x=left切线, y=top切线,
// 宽=图宽-左右切线, 高=图高-上下切线）。
// 越界钳制：用户拖拽红框可能把 text_region 拖到图外（如 y=209 > 图高200），
// 文字会画在图片外面（真机根因 2026-10-10「字不在输入框区域内」），
// 这里统一钳制在图内，渲染/命中测试/测量全部使用钳制后的值。
skin::SkinTextRegion ResolveTextRegion(const skin::Theme& theme, int img_w, int img_h) {
  skin::SkinTextRegion tr = theme.skin.text_region;
  // v4.7.5：≤1 一律视为未设置（含 1×1px 塌缩——真机曾把 text_region 保存成
  // width=1/height=1，导致文字区塌缩、布局分层异常、双击命中失败），
  // 与设置窗 GetEditInt 最小钳制 1→0 配套，走默认推导。
  if (tr.width <= 1 || tr.height <= 1) {
    tr.x = theme.skin.borders.left;
    tr.y = theme.skin.borders.top;
    tr.width = img_w - theme.skin.borders.left - theme.skin.borders.right;
    tr.height = img_h - theme.skin.borders.top - theme.skin.borders.bottom;
    if (tr.width < 1) tr.width = 1;
    if (tr.height < 1) tr.height = 1;
  }
  if (tr.width < 1) tr.width = 1;
  if (tr.height < 1) tr.height = 1;
  if (tr.x < 0) tr.x = 0;
  if (tr.y < 0) tr.y = 0;
  if (tr.x + tr.width > img_w) tr.x = img_w - tr.width;
  if (tr.y + tr.height > img_h) tr.y = img_h - tr.height;
  if (tr.x < 0) {
    tr.x = 0;
    tr.width = img_w;
  }
  if (tr.y < 0) {
    tr.y = 0;
    tr.height = img_h;
  }
  return tr;
}

// 主题颜色 0xAARRGGBB → GDI COLORREF（0x00BBGGRR）
COLORREF ToColorRef(std::uint32_t argb) {
  return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

// 主题颜色 0xAARRGGBB → GDI+ Color（带 alpha，用于半透明高亮块）
Gdiplus::Color ToGdiColor(std::uint32_t argb, BYTE alpha_override = 0xFF) {
  BYTE a = (argb >> 24) & 0xFF;
  if (alpha_override != 0xFF) {
    a = alpha_override;  // 显式覆盖透明度（如高亮块半透明）
  }
  return Gdiplus::Color(a, (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

HFONT CreateThemeFont(const skin::Theme& theme) {
  // family 为 UTF-8，转宽字符
  std::string family = theme.font.family;
  // Windows 垂直字体陷阱：字体名以 '@' 开头（如 "@黑体"）是竖排旋转 90° 的
  // 变体，TextOut 渲染会整体躺倒（真机踩坑：选到 "@Aa厚底黑" 候选字全部旋转）。
  // 成熟做法（Weasel 等）：过滤 '@' 前缀，用同名常规字体。
  if (!family.empty() && family[0] == '@') {
    family = family.substr(1);
  }
  int wlen = MultiByteToWideChar(CP_UTF8, 0, family.c_str(), -1, nullptr, 0);
  std::wstring wfamily(wlen > 1 ? static_cast<std::size_t>(wlen - 1) : 0, L'\0');
  if (wlen > 1) {
    MultiByteToWideChar(CP_UTF8, 0, family.c_str(), -1, &wfamily[0], wlen);
  }
  HFONT font = CreateFontW(theme.font.size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                           wfamily.empty() ? nullptr : wfamily.c_str());
  if (font == nullptr) {
    // 字体创建失败（无效字体名/系统无该字体）→ 系统默认字体兜底。
    // 真机根因 2026-10-03：失败时 SelectObject(NULL) 导致后续
    // GetTextExtentPoint32W 全部返回 0 → 候选窗塌缩成 padding 宽（24x34），
    // 候选文字全部重叠。兜底后测量/渲染始终有有效字体。
    ::ime::LogF( "[candidate] WARN: 字体创建失败(family=%s size=%d)，回退系统字体\n",
                family.c_str(), theme.font.size);
    font = static_cast<HFONT>(GetStockObject(SYSTEM_FONT));
  }
  return font;
}

// 文本测量：失败时按字号估算宽度，绝不返回 0（防止候选窗塌缩成 padding 宽）。
// 真机根因 2026-10-03：GetTextExtentPoint32W 在字体无效时返回 FALSE 且 s 保持
// {0,0}，Measure 算出 total_cx=0 → 窗口只有 padding 宽（24px），候选全叠。
// 中文字符宽度 ≈ 字号，拉丁/数字 ≈ 0.5 字号；加 10% 余量避免贴边。
void MeasureText(HDC dc, const std::wstring& label, int font_size, SIZE* out) {
  out->cx = 0;
  out->cy = font_size + 4;
  if (label.empty()) {
    return;
  }
  SIZE s{0, 0};
  if (GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &s)) {
    out->cx = s.cx > 0 ? s.cx : 1;
    out->cy = s.cy > 0 ? s.cy : out->cy;
    return;
  }
  // 兜底估算：每个字符按 0.75 字号宽（中文约 1 字号宽，拉丁约半宽，折中）
  out->cx = static_cast<int>(static_cast<double>(label.size()) * font_size * 0.75) + 2;
  out->cy = font_size + 4;
}

std::wstring ToWide(const std::string& utf8) {
  int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (len <= 1) {
    return L"";
  }
  std::wstring out(static_cast<std::size_t>(len - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &out[0], len);
  return out;
}

// 皮肤图片绝对路径解析：theme.yaml 里存相对文件名（如 "role.png"）。
// 候选窗/设置面板运行时的当前工作目录是 exe 所在目录，不是配置目录，
// 直接用相对名加载 Bitmap 会失败（真机根因 2026-10-03：上传后无预览、候选窗
// PNG 背景回退纯色）。解析顺序：原字符串已是绝对路径/可直接加载 → 用原串；
// 否则拼 config_dir + 文件名。
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
  // 都加载失败：返回原串（调用方自行判断 Bitmap 状态并回退）
  return direct;
}

// ---- 皮肤图缓存（2026-10-10 卡顿修复）----
// 根因：候选窗每次按键全量重渲染时，Measure/RenderLines/RoleLayer 都从磁盘
// 重新构造 Gdiplus::Bitmap（读文件 + PNG 解码），候选窗 1049px 宽时连续打字
// 明显卡顿。对齐官方逻辑：weasel 官方 UI 的皮肤资源加载一次、绘制复用；
// 搜狗皮肤（官方 FAQ：皮肤图片安装时解压本地、候选框渲染不因图片变慢）也是
// 素材常驻内存。这里用进程内缓存：键=绝对路径，路径变化（换皮肤/换图）自动
// 重新加载，仅 3 张皮肤图，体积有界。
struct SkinBitmapCacheEntry {
  std::wstring path;
  Gdiplus::Bitmap* bmp = nullptr;
};
SkinBitmapCacheEntry g_cache_frame;  // frame_image（输入框整图）
SkinBitmapCacheEntry g_cache_role;   // role_image（角色/月亮图）
SkinBitmapCacheEntry g_cache_bg;     // background_image（纯色模式背景图）

Gdiplus::Bitmap* GetCachedSkinBitmap(const std::wstring& abs_path,
                                     SkinBitmapCacheEntry& cache) {
  if (cache.bmp != nullptr && cache.path == abs_path) {
    return cache.bmp;
  }
  if (cache.bmp != nullptr) {
    delete cache.bmp;
    cache.bmp = nullptr;
  }
  cache.path.clear();
  if (abs_path.empty()) {
    return nullptr;
  }
  Gdiplus::Bitmap* loaded = new Gdiplus::Bitmap(abs_path.c_str());
  if (loaded->GetLastStatus() != Gdiplus::Ok || loaded->GetWidth() < 1) {
    delete loaded;
    return nullptr;
  }
  cache.bmp = loaded;
  cache.path = abs_path;
  return cache.bmp;
}

// 横排候选标签：带序号前缀（weasel 风格 "1.你好 2.您好"），
// 让用户一眼看出当前高亮的是第几个候选、翻页后序号是否归位。
// 序号只在 1-9 内加（max_items 上限 8，足够覆盖）。
std::wstring FormatLabel(int index, const std::string& utf8) {
  std::wstring label = ToWide(utf8);
  if (index >= 0 && index < 9) {
    label = std::to_wstring(index + 1) + L"." + label;
  }
  return label;
}

LRESULT CALLBACK CandidateWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* self = reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;  // 分层窗口由 UpdateLayeredWindow 提供内容
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_NCHITTEST:
      // 分层窗口（WS_EX_LAYERED）鼠标命中按像素 alpha：alpha=0 的区域点击会
      // **穿透到下层窗口**（真机根因 2026-10-04「点击穿过点到下面的应用、
      // 测试界面被盖过去」）。返回 HTCLIENT 让**整个窗口矩形**都接收鼠标，
      // 透明像素也拦截——候选窗矩形贴合内容，拦截不影响操作，杜绝穿透。
      // （真正根治靠 RenderToLayeredWindow 末尾的 DIB alpha 后处理：矩形内
      // alpha 强制 0xFF，系统命中测试全拦截；圆角四角 alpha=0 真透明。）
      return HTCLIENT;
    case WM_LBUTTONDOWN: {
      // 标准输入法候选窗：鼠标点击候选 → 宿主选中并上屏
      if (self != nullptr) {
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
        self->HandleMouseClick(x, y);
      }
      return 0;
    }
    case WM_MOUSEWHEEL: {
      // 标准输入法候选窗：滚轮翻页（上滚 = 上一页，下滚 = 下一页）
      if (self != nullptr) {
        const short delta = static_cast<short>(HIWORD(wParam));
        if (delta != 0) {
          self->HandleMouseWheel(delta > 0);
        }
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      // 双击角色图进入编辑模式
      if (self != nullptr) {
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(HIWORD(lParam));
        self->HandleRoleDoubleClick(x, y);
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      // 编辑模式下拖拽调整角色图大小
      if (self != nullptr) {
        const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
        const int y = static_cast<int>(HIWORD(lParam));
        const bool left_down = (wParam & MK_LBUTTON) != 0;
        self->HandleRoleMouseMove(x, y, left_down);
      }
      return 0;
    }
    case WM_KEYDOWN: {
      // 编辑模式下回车保存
      if (self != nullptr) {
        self->HandleRoleKeyDown(wParam);
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      // 候选窗右键：弹出输入法菜单（托盘同款）。WPS 里语言栏不可靠，
      // 候选窗是唯一常驻的 Beautype UI，右键必须能出菜单（真机 2026-10-13）。
      if (self != nullptr) {
        self->HandleRightClick();
      }
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// role 角色图浮层窗口过程：纯展示层（WS_EX_TRANSPARENT 鼠标穿透），
// 不接收任何鼠标交互——消息被系统直接落到下层窗口/候选窗。只做必要的
// 擦除/绘制空响应（分层窗口由 UpdateLayeredWindow 提供内容）。
LRESULT CALLBACK RoleWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

// 命中测试：与 RenderLines 完全一致的文字区起点与横排布局，
// 窗口坐标落在某候选文字区间内 → 返回该候选下标（-1 = 未命中）。
// （成员函数定义必须位于 ime::ui 命名空间作用域，不能放进上面的匿名命名空间）
int CandidateWindow::HitTestCandidate(int x, int y) const {
  if (candidates_.empty()) {
    return -1;
  }
  int text_ox = theme_.layout.padding_x;
  int text_oy = theme_.layout.padding_y;
  const bool frame_mode =
      theme_.skin.mode == skin::SkinMode::Frame && !theme_.skin.frame_image.empty();
  if (frame_mode) {
    const std::wstring wpath = ResolveSkinPath(theme_.skin.frame_image, config_dir_);
    if (!wpath.empty()) {
      Gdiplus::Bitmap* bmp = GetCachedSkinBitmap(wpath, g_cache_frame);
      if (bmp != nullptr) {
        const skin::SkinTextRegion tr =
            ResolveTextRegion(theme_, bmp->GetWidth(), bmp->GetHeight());
        // v2.1 修复：与 RenderLines 完全一致（text_region 直接值，不叠 padding）
        text_ox = tr.x;
        text_oy = tr.y;
      }
    }
  }
  const int item_h = theme_.layout.item_height;
  // 组合区（preedit）栏已按用户要求移除（2026-10-07，对齐官方：候选窗不显示
  // preedit 行），候选行直接从 text_oy 开始，preedit_h 恒为 0
  const int preedit_h = 0;
  // role 模式：候选框向下偏移（与 RenderLines 一致）
  if (theme_.skin.mode == skin::SkinMode::Role && !theme_.skin.role_image.empty()) {
    const std::wstring rp = ResolveSkinPath(theme_.skin.role_image, config_dir_);
    if (!rp.empty()) {
      Gdiplus::Bitmap* rb = GetCachedSkinBitmap(rp, g_cache_role);
      if (rb != nullptr) {
        const int rh = theme_.skin.role_height;
        const int content_h = preedit_h + item_h + theme_.layout.padding_y * 2;
        const int offset = rh - content_h;
        if (offset > 0) text_oy += offset;
      }
    }
  }
  const int cand_oy =
      text_oy + preedit_h + (preedit_h > 0 ? theme_.layout.item_spacing : 0);
  if (y < cand_oy || y >= cand_oy + item_h) {
    return -1;
  }
  HDC dc = CreateCompatibleDC(nullptr);
  if (dc == nullptr) {
    return -1;
  }
  HFONT font = CreateThemeFont(theme_);
  HGDIOBJ old = SelectObject(dc, font);
  int count = static_cast<int>(candidates_.size());
  if (count > theme_.layout.max_items) {
    count = theme_.layout.max_items;
  }
  int cx = text_ox;
  int hit = -1;
  for (int i = 0; i < count; ++i) {
    const std::wstring label =
        FormatLabel(i, candidates_[static_cast<std::size_t>(i)]);
    SIZE s{0, 0};
    MeasureText(dc, label, theme_.font.size, &s);
    const int item_w = s.cx > 0 ? s.cx : 0;
    if (x >= cx && x < cx + item_w) {
      hit = i;
      break;
    }
    cx += item_w + theme_.layout.item_spacing;
  }
  SelectObject(dc, old);
  DeleteObject(font);
  DeleteDC(dc);
  return hit;
}

void ComputeNinePatch(int img_w, int img_h, int dst_w, int dst_h,
                      const skin::SkinBorders& b, NinePatchRect out[kNinePatchCount]) {
  // 切线夹紧：非负、不超图尺寸、左右/上下不交叉（各边至少留 1px）
  int l = b.left, r = b.right, t = b.top, bb = b.bottom;
  if (l < 0) l = 0;
  if (r < 0) r = 0;
  if (t < 0) t = 0;
  if (bb < 0) bb = 0;
  if (l > img_w - 1) l = img_w - 1;
  if (r > img_w - 1) r = img_w - 1;
  if (t > img_h - 1) t = img_h - 1;
  if (bb > img_h - 1) bb = img_h - 1;
  if (l + r > img_w) {
    r = img_w - l;
    if (r < 0) r = 0;
  }
  if (t + bb >= img_h) {
    // 切线越界（矮图 + 默认 40/40 等）：等比收缩到中间至少 1px。
    // Android/libGDX NinePatch 语义——否则 src_mid_h=0 中间段被渲染跳过，
    // 画面只剩上下边条，视觉上「中间对半裁开」（真机根因 2026-10-04）。
    const int max_tb = img_h - 1;
    if (max_tb < 2) {
      t = 0;
      bb = 0;
    } else {
      const double k = static_cast<double>(max_tb) / (t + bb);
      t = std::max(1, static_cast<int>(t * k));
      bb = max_tb - t;
      if (bb < 1) {
        bb = 1;
        t = max_tb - 1;
      }
    }
  }
  const int src_mid_w = img_w - l - r;
  const int src_mid_h = img_h - t - bb;

  // 目标切线同样夹紧（候选窗可能小于切线，如单候选极窄）。
  // 成熟做法（Android/libGDX NinePatch）：窗口小于固定区总宽/总高时，
  // 固定区（四角+四边）按比例等比收缩，中间文字区永远保留至少 1px；
  // 而不是把固定区硬撑到边界把中间区挤成 0（那样中间段 0 尺寸、渲染被跳过）。
  int dl = l, dr = r, dt = t, db = bb;
  const int fixed_w = dl + dr;
  const int fixed_h = dt + db;
  if (fixed_w > dst_w - 1) {
    const int avail = dst_w - 1;  // 固定区可占的最大宽度，中间至少留 1px
    if (avail < 2) {
      dl = 0;
      dr = 0;  // 极窄窗口：固定区全让给中间 1px，四角/边段 dw=0 由渲染跳过
    } else {
      const double k = static_cast<double>(avail) / fixed_w;
      dl = std::max(1, static_cast<int>(dl * k));
      dr = avail - dl;
      if (dr < 1) {
        dr = 1;
        dl = avail - dr;
      }
    }
  }
  if (fixed_h > dst_h - 1) {
    const int avail = dst_h - 1;
    if (avail < 2) {
      dt = 0;
      db = 0;
    } else {
      const double k = static_cast<double>(avail) / fixed_h;
      dt = std::max(1, static_cast<int>(dt * k));
      db = avail - dt;
      if (db < 1) {
        db = 1;
        dt = avail - db;
      }
    }
  }
  const int mid_w = dst_w - dl - dr;
  const int mid_h = dst_h - dt - db;

  // 0 左上角 / 1 上边 / 2 右上角 / 3 左边 / 4 中间 / 5 右边 / 6 左下角 / 7 下边 / 8 右下角
  out[0] = {0, 0, l, t, 0, 0, dl, dt};
  out[1] = {l, 0, src_mid_w, t, dl, 0, mid_w, dt};
  out[2] = {img_w - r, 0, r, t, dst_w - dr, 0, dr, dt};
  out[3] = {0, t, l, src_mid_h, 0, dt, dl, mid_h};
  out[4] = {l, t, src_mid_w, src_mid_h, dl, dt, mid_w, mid_h};
  out[5] = {img_w - r, t, r, src_mid_h, dst_w - dr, dt, dr, mid_h};
  out[6] = {0, img_h - bb, l, bb, 0, dst_h - db, dl, db};
  out[7] = {l, img_h - bb, src_mid_w, bb, dl, dst_h - db, mid_w, db};
  out[8] = {img_w - r, img_h - bb, r, bb, dst_w - dr, dst_h - db, dr, db};
}

const wchar_t CandidateWindow::kWindowClass[] = L"RimeBeautifyCandidateWindow";
const wchar_t CandidateWindow::kRoleWindowClass[] = L"RimeBeautifyRoleLayer";

CandidateWindow::~CandidateWindow() {
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (role_hwnd_ != nullptr) {
    DestroyWindow(role_hwnd_);
    role_hwnd_ = nullptr;
  }
}

void CandidateWindow::SetTheme(const skin::Theme& theme) {
  theme_ = theme;
  // 创建 DPI 缩放后的主题副本（用于渲染，不污染用户配置）
  // 对齐 weasel 官方 DPI_SCALE 宏：只缩放布局尺寸，不缩放角色图（role_height 用户自定义）
  scaled_theme_ = theme_;
  scaled_theme_.font.size = static_cast<int>(theme_.font.size * dpi_scale_);
  scaled_theme_.layout.padding_x = static_cast<int>(theme_.layout.padding_x * dpi_scale_);
  scaled_theme_.layout.padding_y = static_cast<int>(theme_.layout.padding_y * dpi_scale_);
  scaled_theme_.layout.item_spacing = static_cast<int>(theme_.layout.item_spacing * dpi_scale_);
  scaled_theme_.layout.item_height = static_cast<int>(theme_.layout.item_height * dpi_scale_);
  scaled_theme_.highlight_radius = static_cast<int>(theme_.highlight_radius * dpi_scale_);
  // role_height 不缩放！（用户可拖拽调整，对齐用户要求）
  // 设置面板「保存并应用」热更新：候选窗正显示时立即用新皮肤重绘
  if (hwnd_ != nullptr && IsWindowVisible(hwnd_)) {
    RenderToLayeredWindow();
  }
}

void CandidateWindow::SetConfigDir(const std::string& config_dir) {
  config_dir_ = config_dir;
}

void CandidateWindow::SetSchemaTitle(const std::string& schema_id) {
  if (hwnd_ == nullptr || schema_id.empty()) {
    return;
  }
  const std::wstring ws = ToWide(schema_id);
  if (!ws.empty()) {
    SetWindowTextW(hwnd_, ws.c_str());
  }
}

// v4.7.5 候选窗 workarea 钳制（对齐官方 WeaselPanel::_RepositionWindow
// 1222-1292 的 keep-visible 逻辑）：尺寸已知后在渲染落位执行。
// 根因：SetPosition 的旧钳制依赖 size_cache_（窗口尺寸），但服务端先发位置
// 后算尺寸，收到坐标时 size_cache_ 还是 0 → 钳制被跳过 → pos=(1605,1356)
// 超屏（屏幕仅 1080 高），候选窗画到屏幕外/不在光标下（真机 2026-10-12）。
// 这里在 RenderToLayeredWindow 算完 size 后补钳制并写回 pos_x_/pos_y_，
// UpdateLayeredWindow 的落位与角色浮层锚点全部使用钳制后的坐标。
void ClampPosToWorkArea(int* x, int* y, SIZE size) {
  if (size.cx <= 0 || size.cy <= 0) {
    return;
  }
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfo(MonitorFromPoint(POINT{*x, *y}, MONITOR_DEFAULTTONEAREST),
                     &mi)) {
    if (*y > mi.rcWork.bottom - size.cy) {
      *y = mi.rcWork.bottom - size.cy;
    }
    if (*y < mi.rcWork.top) {
      *y = mi.rcWork.top;
    }
    if (*x > mi.rcWork.right - size.cx) {
      *x = mi.rcWork.right - size.cx;
    }
    if (*x < mi.rcWork.left) {
      *x = mi.rcWork.left;
    }
  }
}

void CandidateWindow::SetPosition(int x, int y, int caret_top) {
  // 位置去重（对齐官方 WeaselPanel::MoveTo 的防抖语义，WeaselPanel.cpp:
  // 1176-1219）：官方在「left 相同且 Δbottom<6」时不重定位（注释原话：
  // word 2021 的 bottom 会 1-2px 抖动，防闪烁）；Beautype 在此语义上保留
  // 水平 2px 容差（2026-10-10 渲染风暴实测：TSF 布局 ±1~2px 抖动每次触发
  // 全窗口 UpdateLayeredWindow → 输入框右侧打字卡顿）。视觉上 6px 内无感，
  // 微软输入法候选窗同样只在位置明显变化时移动。
  if (std::abs(x - pos_x_) <= 2 && std::abs(y - pos_y_) < 6) {
    return;
  }
  // 官方 m_sticky 重置（MoveTo 1176-1192）：输入会话结束（候选空）或位置
  // 大幅变化（>50px）时取消"翻到上方"记忆，强制回到光标下方重新判定。
  if (sticky_ && (candidates_.empty() || std::abs(x - pos_x_) > 50 ||
                  std::abs(y - pos_y_) > 50)) {
    sticky_ = false;
  }
  // 屏幕边界修正 + 底部翻转（对齐官方 WeaselPanel::_RepositionWindow
  // 1222-1292）：候选窗不超出工作区右/下边界；屏幕底部光标处放不下时
  // 翻到光标上方并记忆 sticky（防弹跳），不再"硬压回工作区底缘"导致
  // 候选窗离光标很远（此前 Beautype 只有钳制、缺翻转——官方成熟行为）。
  if (size_cache_.cx > 0 || size_cache_.cy > 0) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(
            MonitorFromPoint(POINT{x, y}, MONITOR_DEFAULTTONEAREST), &mi)) {
      // 官方 keep-visible：工作区先减去窗口尺寸，再判窗口顶是否越界。
      // 官方条件 y > rcWorkArea.bottom（=工作区底-height），窗口顶越界 →
      // 翻到光标上方：y = caret_top - height - 6（官方 m_inputPos.top -
      // height - 6；Beautype 有月亮高差 offset，候选框底贴光标 top 上方 6px）
      if (size_cache_.cy > 0 &&
          (y > mi.rcWork.bottom - size_cache_.cy || sticky_)) {
        if (!sticky_)
          sticky_ = true;
        y = caret_top - 6 - size_cache_.cy - GetRoleOffsetY();
      }
      if (y < mi.rcWork.top) {
        y = mi.rcWork.top;
      }
      if (size_cache_.cx > 0 && x > mi.rcWork.right - size_cache_.cx) {
        x = mi.rcWork.right - size_cache_.cx;
      }
      if (x < mi.rcWork.left) {
        x = mi.rcWork.left;
      }
    }
  }
  pos_x_ = x;
  pos_y_ = y;
  has_position_ = true;
  // 位置就绪：补弹挂起的候选窗；窗口已显示则立即重定位到新坐标。
  // （修复 2026-10-07：原实现只存坐标，候选窗停留在旧位置/首次在 (0,0) 闪现）
  if (pending_show_ && !candidates_.empty()) {
    pending_show_ = false;
    Show(candidates_, highlighted_);
  } else if (hwnd_ != nullptr && ::IsWindow(hwnd_) && ::IsWindowVisible(hwnd_)) {
    // 重定位：官方同款 SetWindowPos（SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOREDRAW）——
    // 只改位置不触发重绘；窗口内容位置由后续 RenderToLayeredWindow 的
    // UpdateLayeredWindow(pt) 决定。
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
    RenderToLayeredWindow();
    // 重定位时同步月亮浮层位置（ShowRoleLayer 内部重算锚点；非 Role 自动隐藏）
    ShowRoleLayer();
  }
}

int CandidateWindow::GetRoleOffsetY() const {
  // 非 role 模式没有偏移
  if (theme_.skin.mode != skin::SkinMode::Role || theme_.skin.role_image.empty()) {
    return 0;
  }
  // 候选框内容高度用缩放后的 item_height 和 padding_y
  const int content_h = scaled_theme_.layout.item_height + scaled_theme_.layout.padding_y * 2;
  // role_height 不缩放（用户自定义）
  const int offset = theme_.skin.role_height - content_h;
  return offset > 0 ? offset : 0;
}

void CandidateWindow::Show(const std::vector<std::string>& candidates, int highlighted) {
  // 兜底：未注入主题时用默认主题渲染（零值 theme 会把布局尺寸算成 0，窗口不可见）
  if (theme_.font.size <= 0) {
    theme_ = skin::DefaultTheme();
  }
  candidates_ = candidates;
  highlighted_ = highlighted;
  // 位置未就绪（服务端尚未收到 DLL 上报的坐标）：挂起，等 SetPosition 再弹，
  // 避免候选窗以初始 (0,0) 在左上角闪现（修复 2026-10-07）
  if (!has_position_) {
    pending_show_ = true;
    return;
  }
  pending_show_ = false;
  EnsureWindow();
  RenderToLayeredWindow();
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  UpdateWindow(hwnd_);
  // role 月亮浮层：主窗显示后同步弹浮层（修复 2026-10-13：a0c71d1fa 拆浮层时
  // 未在 Show() 接线，角色图从 v4.7.5 起从未显示；ShowRoleLayer 内部对非 Role
  // 模式自动隐藏，所有模式安全）
  ShowRoleLayer();
  // 防御日志：候选窗弹窗路径（真机「保存后候选框不显示」用此行定位）
  ::ime::LogF( "[candidate] Show(candidates=%d, highlight=%d) size=%dx%d pos=(%d,%d)\n",
              static_cast<int>(candidates.size()), highlighted,
              size_cache_.cx, size_cache_.cy, pos_x_, pos_y_);
}

void CandidateWindow::Hide() {
  pending_show_ = false;
  if (hwnd_ != nullptr && ::IsWindow(hwnd_)) {
    ShowWindow(hwnd_, SW_HIDE);
  }
  HideRoleLayer();
}

void CandidateWindow::CreateMainWindow() {
  // 主线程预创建（对齐官方 weasel：窗口归消息泵线程）。两个窗口（候选窗 +
  // role 月亮浮层）都在这里创建；此后 pipe 线程的 Show/Hide/SetPosition 跨线程
  // 窗口操作由主线程消息循环处理，不会等待一个阻塞在管道读取的线程（真机根因
  // 2026-10-07：悬停月亮 → 沙漏 → 输入框卡死）。
  EnsureWindow();
  EnsureRoleWindow();
}

void CandidateWindow::EnsureWindow() {
  // 防御：窗口可能被系统/外部销毁（句柄悬垂）→ 重建（真机“输入框按不出来”）
  if (hwnd_ != nullptr && ::IsWindow(hwnd_)) {
    return;
  }
  hwnd_ = nullptr;

  // v5.x：不做 DPI 缩放（dpi_scale_ 恒 1.0）。候选窗尺寸固定（默认字号 40/
  // 条目高 54，theme.yaml 优先），所有屏幕统一，无"SetTheme 先于 DPI 初始化
  // 导致快照系数 1.0 定格"的时序问题（历史：06ad0fc08 动态→静态转折点）。
  // 如需恢复缩放：恢复此处 LoadLibrary+GetDpiForMonitor 并刷新 scaled_theme_。

  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = CandidateWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    if (RegisterClassExW(&wc) != 0) {
      registered = true;
    }
  }
  hwnd_ = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      kWindowClass, L"RimeBeautifyCandidate", WS_POPUP, pos_x_, pos_y_, 1, 1,
      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (hwnd_ != nullptr) {
    // 窗口过程取回实例做命中测试（鼠标点选/滚轮翻页；标准输入法候选窗交互）
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  }
}

SIZE CandidateWindow::Measure(const skin::Theme& theme,
                              const std::vector<std::string>& candidates,
                              const std::string& config_dir,
                              const std::string& preedit) {
  (void)preedit;  // preedit 栏已移除（2026-10-07），参数保留 API 兼容
  SIZE size{1, 1};
  if (candidates.empty()) {
    return size;
  }
  HDC dc = CreateCompatibleDC(nullptr);
  if (dc == nullptr) {
    return size;
  }
  HFONT font = CreateThemeFont(theme);
  HGDIOBJ old = SelectObject(dc, font);
  // 横排布局：所有候选水平排成一行（带序号），窗口宽 = Σ(项宽+间隔)，高 = 行高
  int count = static_cast<int>(candidates.size());
  if (count > theme.layout.max_items) {
    count = theme.layout.max_items;
  }
  int total_cx = 0;
  int max_text_cy = 0;
  for (int i = 0; i < count; ++i) {
    std::wstring label = FormatLabel(i, candidates[static_cast<std::size_t>(i)]);
    SIZE s{0, 0};
    MeasureText(dc, label, theme.font.size, &s);
    total_cx += s.cx;
    if (s.cy > max_text_cy) {
      max_text_cy = s.cy;
    }
    if (i < count - 1) {
      total_cx += theme.layout.item_spacing;
    }
  }
  // 组合区（preedit）栏已移除（2026-10-07，对齐官方：候选窗不显示 preedit 行），
  // 不再参与窗口尺寸计算（preedit_h 恒为 0）
  const int preedit_h = 0;
  SelectObject(dc, old);
  DeleteObject(font);
  DeleteDC(dc);

  int cy = theme.layout.item_height;
  if (max_text_cy > cy) {
    cy = max_text_cy;  // 兜底：字号大于行高时以字高为准
  }
  size.cx = total_cx + theme.layout.padding_x * 2;
  size.cy = cy + theme.layout.padding_y * 2 + preedit_h;
  // role 模式：右边多留 80px 空白给月亮遮挡，候选词不贴边
  if (theme.skin.mode == skin::SkinMode::Role && !theme.skin.role_image.empty()) {
    size.cx += 80;
  }
  if (size.cx < 1) size.cx = 1;
  if (size.cy < 1) size.cy = 1;

  // v3 frame 模式：窗口 = 图片原始尺寸（文字短时保持猫咪框完整不变形），
  // 候选文字超过图片宽/高时才随文字增长（中间区拉伸，避免文字截断）。
  // 修复真机根因 2026-10-10「按回车候选窗变得巨宽」：旧公式
  // size.cx = 切线left + max(文字宽, 文字区宽) + 切线right，短候选也会被
  // 300px 固定切线撑到 725~1369px 宽；现改为 max(图宽, 文字总宽+边距)。
  if (theme.skin.mode == skin::SkinMode::Frame && !theme.skin.frame_image.empty()) {
    const std::wstring wpath = ResolveSkinPath(theme.skin.frame_image, config_dir);
    if (!wpath.empty()) {
      Gdiplus::Bitmap* bmp = GetCachedSkinBitmap(wpath, g_cache_frame);
      if (bmp != nullptr) {
        const int img_w = bmp->GetWidth();
        const int img_h = bmp->GetHeight();
        if (img_w > 0 && img_h > 0) {
          if (size.cx < img_w) size.cx = img_w;
          if (size.cy < img_h) size.cy = img_h;
        }
        if (size.cx < 1) size.cx = 1;
        if (size.cy < 1) size.cy = 1;
        return size;
      }
    }
  }
  // v3 role 模式：月亮压在候选框右上角，候选框高度不变，底部和月亮对齐。
  // 候选框右边延伸到月亮中心（重叠月亮宽度的一半）。
  if (theme.skin.mode == skin::SkinMode::Role && !theme.skin.role_image.empty()) {
    const std::wstring wpath = ResolveSkinPath(theme.skin.role_image, config_dir);
    if (!wpath.empty()) {
      Gdiplus::Bitmap* bmp = GetCachedSkinBitmap(wpath, g_cache_role);
      if (bmp != nullptr) {
        const int role_h = theme.skin.role_height;
        const int role_w = (int)((int64_t)bmp->GetWidth() * role_h / bmp->GetHeight());
        const int overlap = (int)(role_w * 0.5);  // 候选框延伸到月亮中心
        size.cx = size.cx + role_w - overlap;  // 总宽 = 候选框宽 + 月亮宽 - 重叠
        if (role_h > size.cy) {
          size.cy = role_h;  // 总高 = 月亮高（候选框向下偏移，月亮向上溢出）
        }
      }
    }
  }
  return size;
}

bool CandidateWindow::RenderLines(HDC dc, int width, int height,
                                  const std::vector<std::string>& candidates,
                                  int highlighted) const {
  if (dc == nullptr || width <= 0 || height <= 0 || candidates.empty()) {
    return false;
  }
  int count = static_cast<int>(candidates.size());
  if (count > theme_.layout.max_items) {
    count = theme_.layout.max_items;
  }

  // 1) 背景（v3 frame=9 段整图 / 其余：PNG 铺满或纯色）+ 高亮块（GDI+）
  const bool frame_mode =
      theme_.skin.mode == skin::SkinMode::Frame && !theme_.skin.frame_image.empty();
  // 文字排版基准：frame 模式在 text_region 内，其余沿用 padding
  int text_ox = theme_.layout.padding_x;
  int text_oy = theme_.layout.padding_y;
  // role 模式：候选框内容向下偏移，让候选框底部和月亮底部对齐
  int content_h_offset = 0;
  if (theme_.skin.mode == skin::SkinMode::Role && !theme_.skin.role_image.empty()) {
    const std::wstring rp = ResolveSkinPath(theme_.skin.role_image, config_dir_);
    if (!rp.empty()) {
      Gdiplus::Bitmap* rb = GetCachedSkinBitmap(rp, g_cache_role);
      if (rb != nullptr) {
        const int rh = theme_.skin.role_height;
        // 候选框内容高度 = item_height + 上下 padding（preedit 栏已移除，恒为 0）
        const int preedit_h_tmp = 0;
        const int content_h = preedit_h_tmp + theme_.layout.item_height + theme_.layout.padding_y * 2;
        content_h_offset = rh - content_h;  // 向下偏移量
        if (content_h_offset < 0) content_h_offset = 0;
        text_oy += content_h_offset;
      }
    }
  }
  // 组合区（preedit）行已移除（2026-10-07，对齐官方）：候选行起点 = text_oy
  const int preedit_h = 0;
  // v2.1 根因修复：cand_oy 不能在 frame 分支覆盖 text_oy 之前固化——
  // frame 分支会把 text_oy 从 padding_y=10 改成 tr.y，若 cand_oy 用初始值，
  // 高亮块和文字都画在 y≈10 顶部黑色区（真机根因：拖红框文字纹丝不动）。
  // 改成 int，frame 分支覆盖 text_oy 后重新算 cand_oy。
  int cand_oy =
      text_oy + preedit_h + (preedit_h > 0 ? theme_.layout.item_spacing : 0);
  {
    Gdiplus::Graphics g(dc);
    // v5.2 黑线修复（用户实测：浅色模式候选窗左/上边缘灰黑线）：
    // 抗锯齿会让 FillRectangle/DrawImage 的矩形四边产生半透明边缘像素，
    // 分层窗口与桌面合成后成灰黑过渡线；2026-10-06 的"全 0 跳过"后处理
    // 堵不住它（半透明像素 RGB≠0 不满足跳过条件）。背景是规则几何，
    // 不需要抗锯齿 → 用默认（None）绘制，边缘像素 alpha 精确 0xFF；
    // 抗锯齿只给下方圆角高亮块（FillPath 前重开）。
    bool bg_drawn = false;
    if (frame_mode) {
      const std::wstring wpath = ResolveSkinPath(theme_.skin.frame_image, config_dir_);
      if (!wpath.empty()) {
        Gdiplus::Bitmap* bmp = GetCachedSkinBitmap(wpath, g_cache_frame);
        if (bmp != nullptr) {
          const int img_w = bmp->GetWidth();
          const int img_h = bmp->GetHeight();
          const skin::SkinTextRegion tr = ResolveTextRegion(theme_, img_w, img_h);
          // v2.1 修复：文字起点 = text_region 直接值，不再叠加 padding——
          // text_region 是用户拖拽红框对准的「图片内文字区」，叠加 padding 会让
          // 文字比红框偏右下 (padding_x,padding_y)（真机：拖好红框文字仍错位）。
          // padding 只服务无图纯色模式（text_ox/oy 初值即 padding）。
          text_ox = static_cast<int>(tr.x);
          // v2.1 垂直居中：红框高 tr.height 远大于单行高 item_height，文字从红框
          // 顶边开始会被顶在红框最上。改为红框内垂直居中——用户拖红框对准输入框
          // 长条，文字自动落在长条中间（真机：红框贴底时文字仍在长条上方）。
          const int line_h = theme_.layout.item_height;
          text_oy = static_cast<int>(tr.y) +
                    (tr.height > line_h ? (static_cast<int>(tr.height) - line_h) / 2 : 0);
          // v2.1 根因修复：text_oy 被 frame 分支覆盖后，必须重新算 cand_oy，
          // 否则高亮块/文字仍用覆盖前的旧值 padding_y（真机根因）
          cand_oy = text_oy + preedit_h + (preedit_h > 0 ? theme_.layout.item_spacing : 0);
          NinePatchRect segs[kNinePatchCount];
          ComputeNinePatch(img_w, img_h, width, height, theme_.skin.borders, segs);
          g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
          for (int k = 0; k < kNinePatchCount; ++k) {
            const NinePatchRect& s = segs[k];
            if (s.sw <= 0 || s.sh <= 0 || s.dw <= 0 || s.dh <= 0) {
              continue;
            }
            // DrawImage 不存在"9 参数 INT 全表 + Unit"重载（官方 SDK gdiplusgraphics.h
            // 只有 Rect 目标 + INT src 版本与 RectF+RectF+Unit 版本）；用标准九宫格写法
            const Gdiplus::Rect dst_rect(s.dx, s.dy, s.dw, s.dh);
            g.DrawImage(bmp, dst_rect, s.sx, s.sy, s.sw, s.sh, Gdiplus::UnitPixel);
          }
          bg_drawn = true;
        }
      }
    }
    if (!bg_drawn) {
      if (!theme_.background_image.empty() && !frame_mode) {
        const std::wstring wpath = ResolveSkinPath(theme_.background_image, config_dir_);
        if (!wpath.empty()) {
          Gdiplus::Bitmap* bmp = GetCachedSkinBitmap(wpath, g_cache_bg);
          if (bmp != nullptr) {
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(bmp, 0, 0, width, height);  // 铺满候选窗
            bg_drawn = true;
          }
        }
      }
      if (!bg_drawn) {
        // 纯色背景：候选框是完整矩形，画在窗口下方；右边和上边溢出区透明
        int bg_x = 0, bg_y = 0, bg_w = width, bg_h = height;
        if (theme_.skin.mode == skin::SkinMode::Role && !theme_.skin.role_image.empty()) {
          const std::wstring rp = ResolveSkinPath(theme_.skin.role_image, config_dir_);
          if (!rp.empty()) {
            Gdiplus::Bitmap* rb = GetCachedSkinBitmap(rp, g_cache_role);
            if (rb != nullptr) {
              const int rh = theme_.skin.role_height;
              const int rw = (int)((int64_t)rb->GetWidth() * rh / rb->GetHeight());
              const int overlap = (int)(rw * 0.5);  // 候选框延伸到月亮中心
              bg_w = width - rw + overlap;  // 候选框宽度
              // 候选框位置：候选框底部跟月亮底部对齐（原样）
              const int preedit_h_tmp = 0;
              const int content_h = preedit_h_tmp + theme_.layout.item_height + theme_.layout.padding_y * 2;
              bg_y = rh - content_h;
              if (bg_y < 0) bg_y = 0;
              bg_h = content_h;
            }
          }
        }
        g.Clear(Gdiplus::Color(0, 255, 255, 255));  // 先清成全透明
        Gdiplus::SolidBrush bg_brush(ToGdiColor(theme_.colors.background));
        g.FillRectangle(&bg_brush, bg_x, bg_y, bg_w, bg_h);  // 只画候选框矩形
      }
    }
    // role 月亮图已拆为独立浮层窗口（2026-10-04）：不再画在候选窗矩形内
    // （会被矩形裁切压扁、被 preedit 白块挡住），改由 ShowRoleLayer() 单独
    // 渲染到鼠标穿透的独立分层窗口，锚候选窗右上角溢出。
    // 高亮候选块：圆角矩形（颜色取自主题；alpha 尊重主题
    // highlight_background 的 A 通道——2026-10-22 修复：原写死 0xCC 半透明，
    // 纯色模式下透出下层窗口内容、文字发糊（用户实测"候选窗半透明淡紫、
    // 字看不清"）；默认 #FF 全不透明，如需半透明可手改 theme.yaml 的 alpha）
    // 圆角曲线需要抗锯齿平滑（背景已用无抗锯齿绘制，此处重开）
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    if (highlighted >= 0 && highlighted < count) {
      const int item_h = theme_.layout.item_height;
      // 【根因 2026-10-17】高亮块 X 必须按 highlighted 索引累加（与下方文字
      // 循环同规则）：原实现 x 恒为 text_ox → 方向键移动候选时高光背景一直
      // 停在第一个候选（文字颜色已跟随、背景不跟随，用户实机反馈）。
      // 二次根因（同日）：此处位于 GDI+ 阶段，dc 尚未 SelectObject 主题字体
      // （1011 才选），MeasureText 用默认字体测量宽度 ≠ 文字循环实际绘制
      // 宽度 → 高亮块 x 偏左（用户实测停在候选 1 右半）。先临时选中主题字体
      // 再测量，保证 x/item_w 与文字循环同字体上下文。
      HFONT hl_font = CreateThemeFont(theme_);
      HGDIOBJ hl_old = SelectObject(dc, hl_font);
      int x = text_ox;
      for (int i = 0; i < highlighted; ++i) {
        std::wstring lb = FormatLabel(i, candidates[static_cast<std::size_t>(i)]);
        SIZE ls{0, 0};
        MeasureText(dc, lb, theme_.font.size, &ls);
        x += (ls.cx > 0 ? ls.cx : 0) + theme_.layout.item_spacing;
      }
      const int y = cand_oy;
      std::wstring label = FormatLabel(highlighted, candidates[static_cast<std::size_t>(highlighted)]);
      SIZE s{0, 0};
      MeasureText(dc, label, theme_.font.size, &s);
      const int item_w = s.cx > 0 ? s.cx : 0;
      SelectObject(dc, hl_old);
      DeleteObject(hl_font);
      Gdiplus::SolidBrush hl(ToGdiColor(theme_.colors.highlight_background));
      const int r = static_cast<int>(theme_.highlight_radius);
      const int rr = (r * 2 > item_w || r * 2 > item_h)
                         ? (std::min)(item_w, item_h) / 2
                         : r;
      if (rr > 0) {
        // 圆角矩形：GDI+ 无 FillRoundRect，用 GraphicsPath 拼接四段圆弧
        Gdiplus::GraphicsPath path;
        path.AddArc(x, y, rr * 2, rr * 2, 180, 90);
        path.AddArc(x + item_w - rr * 2, y, rr * 2, rr * 2, 270, 90);
        path.AddArc(x + item_w - rr * 2, y + item_h - rr * 2, rr * 2, rr * 2, 0, 90);
        path.AddArc(x, y + item_h - rr * 2, rr * 2, rr * 2, 90, 90);
        path.CloseFigure();
        g.FillPath(&hl, &path);
      } else {
        g.FillRectangle(&hl, x, y, item_w, item_h);
      }
    }
  }

  // 组合区（preedit）行渲染已移除（2026-10-07，用户要求 + 对齐官方：候选窗
  // 不显示 preedit 栏；preedit_h 恒为 0，此块不再执行）

  // 2) 候选文字（GDI 绘制；GDI+ Graphics 已析构，HDC 状态已恢复）
  HFONT font = CreateThemeFont(theme_);
  HGDIOBJ old_font = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);

  // 横排布局：候选水平排列，高亮项高亮块已在 GDI+ 阶段绘制
  const int item_h = theme_.layout.item_height;
  int x = text_ox;
  for (int i = 0; i < count; ++i) {
    std::wstring label = FormatLabel(i, candidates[static_cast<std::size_t>(i)]);
    SIZE s{0, 0};
    MeasureText(dc, label, theme_.font.size, &s);
    const int item_w = s.cx > 0 ? s.cx : 0;
    SetTextColor(dc, ToColorRef(i == highlighted ? theme_.colors.highlight_text
                                                 : theme_.colors.text));
    if (!label.empty()) {
      const int text_y = cand_oy + (item_h - s.cy) / 2;  // 垂直居中
      TextOutW(dc, x, text_y, label.c_str(), static_cast<int>(label.size()));
    }
    x += item_w + theme_.layout.item_spacing;
  }

  SelectObject(dc, old_font);
  DeleteObject(font);
  return true;
}

bool CandidateWindow::RenderToMemoryDib(const std::vector<std::string>& candidates,
                                        int highlighted, int width, int height,
                                        std::uint8_t* argb) const {
  if (argb == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  HDC mem = CreateCompatibleDC(nullptr);
  if (mem == nullptr) {
    return false;
  }
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = width;
  bi.bmiHeader.biHeight = -height;  // top-down
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp == nullptr || bits == nullptr) {
    DeleteDC(mem);
    return false;
  }
  HGDIOBJ old = SelectObject(mem, bmp);
  bool ok = RenderLines(mem, width, height, candidates, highlighted);
  if (ok) {
    // DIB 位图内存在 DeleteObject 后失效：先拷贝再清理
    std::memcpy(argb, bits, static_cast<std::size_t>(width) * height * 4u);
  }
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  if (!ok) {
    return false;
  }

  // 统计非背景像素：证明候选文本真实渲染出来
  const COLORREF bg = ToColorRef(theme_.colors.background);
  const std::uint8_t bg_b = GetBValue(bg);
  const std::uint8_t bg_g = GetGValue(bg);
  const std::uint8_t bg_r = GetRValue(bg);
  const std::size_t total = static_cast<std::size_t>(width) * height;
  for (std::size_t i = 0; i < total; ++i) {
    const std::uint8_t* px = argb + i * 4u;
    if (px[0] != bg_b || px[1] != bg_g || px[2] != bg_r) {
      return true;
    }
  }
  return false;
}

// GDI+ 图像编码器查找（Bitmap::Save 需要 CLSID；MIME 精确匹配 "image/png"）
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid, int* pCount) {
  UINT num = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&num, &size);
  if (size == 0) {
    return -1;
  }
  std::vector<BYTE> buf(size);
  auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
  Gdiplus::GetImageEncoders(num, size, info);
  for (UINT j = 0; j < num; ++j) {
    if (wcsncmp(info[j].MimeType, format, 32) == 0) {
      *pClsid = info[j].Clsid;
      if (pCount != nullptr) {
        *pCount = static_cast<int>(j + 1);
      }
      return 0;
    }
  }
  return -1;
}

bool CandidateWindow::RenderToPng(const std::string& utf8_path,
                                      const std::vector<std::string>& candidates,
                                      int highlighted) const {
  // 离屏渲染验证工具（不创建窗口）：用当前主题把候选渲染成 PNG 文件。
  // 用于真机问题定位（文字截断/布局），避免在用户桌面擅自弹窗。
  SIZE size = Measure(theme_, candidates, config_dir_, preedit_);
  if (size.cx < 1) size.cx = 1;
  if (size.cy < 1) size.cy = 1;
  ::ime::LogF( "[probe] RenderToPng size=%dx%d cands=%zu hl=%d\n",
              size.cx, size.cy, candidates.size(), highlighted);
  std::vector<std::uint8_t> argb(static_cast<std::size_t>(size.cx) * size.cy * 4u);
  if (!RenderToMemoryDib(candidates, highlighted, size.cx, size.cy, argb.data())) {
    ::ime::LogF( "[probe] FAIL: RenderToMemoryDib\n");
    for (int i = 0; i < 16 && i < size.cx * size.cy; ++i) {
      const auto* px = argb.data() + i * 4u;
      ::ime::LogF( "[probe]   px[%d] = B=%u G=%u R=%u A=%u\n", i, px[0], px[1],
                  px[2], px[3]);
    }
    return false;
  }
  Gdiplus::Bitmap bmp(size.cx, size.cy, size.cx * 4, PixelFormat32bppARGB,
                      argb.data());
  const std::wstring wpath = ToWide(utf8_path);
  if (wpath.empty()) {
    return false;
  }
  CLSID enc{};
  int n = 0;
  const int ec = GetEncoderClsid(L"image/png", &enc, &n);
  ::ime::LogF( "[probe] GetEncoderClsid=%d n=%d bmpStatus=%d\n", ec, n,
               static_cast<int>(bmp.GetLastStatus()));
  if (ec == 0 && n > 0) {
    const Gdiplus::Status st = bmp.Save(wpath.c_str(), &enc, nullptr);
    ::ime::LogF( "[probe] Save status=%d\n", static_cast<int>(st));
    return st == Gdiplus::Ok;
  }
  return false;
}

void CandidateWindow::RenderToLayeredWindow() {
  if (hwnd_ == nullptr) {
    return;
  }
  // 用缩放后的主题测量尺寸和渲染
  SIZE size = Measure(scaled_theme_, candidates_, config_dir_, preedit_);
  if (size.cx < 1) size.cx = 1;
  if (size.cy < 1) size.cy = 1;
  size_cache_ = size;
  // v4.7.5：尺寸已知后在此补 workarea 钳制（SetPosition 收到坐标时 size_cache_
  // 可能为 0 被跳过 → 候选窗超屏画到屏幕外；见 ClampPosToWorkArea 注释）。
  // 钳制结果写回 pos_x_/pos_y_：后续 UpdateLayeredWindow 落位与角色浮层锚点
  // 均用钳制后坐标，与官方 _RepositionWindow 的 keep-visible 语义一致。
  ClampPosToWorkArea(&pos_x_, &pos_y_, size_cache_);

  HDC mem = CreateCompatibleDC(nullptr);
  if (mem == nullptr) {
    return;
  }
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = size.cx;
  bi.bmiHeader.biHeight = -size.cy;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp == nullptr) {
    DeleteDC(mem);
    return;
  }
  HGDIOBJ old = SelectObject(mem, bmp);
  // 显式清零：未绘制区域保持 alpha=0 透明，避免残留垃圾像素被后处理
  // 强制成不透明黑线（真机根因 2026-10-06：候选窗左/上边缘 1px 黑边）。
  if (bits != nullptr) {
    std::memset(bits, 0, static_cast<std::size_t>(size.cx) * size.cy * 4u);
  }
  // RenderLines 是成员函数，内部用 theme_，需要临时换成 scaled_theme_
  skin::Theme saved_theme = theme_;
  theme_ = scaled_theme_;
  RenderLines(mem, size.cx, size.cy, candidates_, highlighted_);
  theme_ = saved_theme;

  // 画月亮在右上角（用原始 theme_，role_height 不缩放）
  if (theme_.skin.mode == skin::SkinMode::Role && !theme_.skin.role_image.empty()) {
    const std::wstring role_path = ResolveSkinPath(theme_.skin.role_image, config_dir_);
    if (!role_path.empty()) {
      Gdiplus::Bitmap* role_bmp = GetCachedSkinBitmap(role_path, g_cache_role);
      if (role_bmp != nullptr) {
        const int role_h = theme_.skin.role_height;
        const int role_w = (int)((int64_t)role_bmp->GetWidth() * role_h / role_bmp->GetHeight());
        // 月亮位置：压候选框右上角，加上用户调整的 x 偏移
        const int role_x = size.cx - role_w + role_offset_x_;
        const int role_y = 0;  // 顶部对齐（向上溢出）
        Gdiplus::Graphics g(mem);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(role_bmp, Gdiplus::Rect(role_x, role_y, role_w, role_h),
                    0, 0, role_bmp->GetWidth(), role_bmp->GetHeight(), Gdiplus::UnitPixel);
      }
    }
  }

  // DIB alpha post-processing (2026-10-04 real-device root cause):
  // native GDI (FillRect/TextOut) writing to a 32bpp layered DIB does not
  // guarantee correct alpha bytes (glyph output sets glyph pixels alpha=0),
  // so visually opaque areas could have alpha=0 and the system hit-test
  // passed mouse clicks through to the window below. Force alpha here:
  // inside rounded rect -> 0xFF (solid board, blocks all clicks); outside
  // corners -> 0 (true transparent, naturally rounded corners).
  if (bits != nullptr) {
    const int w = size.cx, h = size.cy;
    int r = scaled_theme_.highlight_radius;  // 用缩放后的圆角半径
    if (r < 0) r = 0;
    // 候选框区域：圆角矩形；月亮溢出区：保持原 alpha
    int box_x0 = 0, box_y0 = 0, box_x1 = w, box_y1 = h;
    if (theme_.skin.mode == skin::SkinMode::Role && !theme_.skin.role_image.empty()) {
      const std::wstring rp = ResolveSkinPath(theme_.skin.role_image, config_dir_);
      if (!rp.empty()) {
        Gdiplus::Bitmap* rb = GetCachedSkinBitmap(rp, g_cache_role);
        if (rb != nullptr) {
          const int rh = theme_.skin.role_height;
          const int rw = (int)((int64_t)rb->GetWidth() * rh / rb->GetHeight());
          const int overlap = (int)(rw * 0.5);  // 候选框延伸到月亮中心
          box_x1 = w - rw + overlap;  // 候选框右边缘
          // 候选框上边缘（preedit 栏已移除，恒为 0）
          const int preedit_h_tmp = 0;
          // 用缩放值：候选框由 RenderLines（scaled_theme_）绘制，box 高度必须与
          // 实际绘制一致，否则高 DPI 下 box 与候选框错位（对齐 v4.7.6 做法）
          const int content_h = preedit_h_tmp + scaled_theme_.layout.item_height + scaled_theme_.layout.padding_y * 2;
          box_y0 = rh - content_h;
          if (box_y0 < 0) box_y0 = 0;
          box_y1 = box_y0 + content_h;
        }
      }
    }
    auto* px = static_cast<std::uint8_t*>(bits);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        // 不在候选框区域内：跳过（月亮区/溢出区保持原 alpha）
        if (x < box_x0 || x >= box_x1 || y < box_y0 || y >= box_y1) continue;
        bool inside = true;
        // 左上角、左下角永远画圆角；右上角、右下角：无角色图时画，有角色图时不画（被角色图挡住）
        const bool has_role = (!theme_.skin.role_image.empty());  // 只要没上传角色图，就永远四个圆角
        if (r > 0 && x < box_x0 + r && y < box_y0 + r) {
          const int dx = box_x0 + r - x, dy = box_y0 + r - y; inside = dx*dx + dy*dy <= r*r;
        } else if (!has_role && r > 0 && x >= box_x1 - r && y < box_y0 + r) {
          const int dx = x - (box_x1 - 1 - r), dy = box_y0 + r - y; inside = dx*dx + dy*dy <= r*r;
        } else if (r > 0 && x < box_x0 + r && y >= box_y1 - r) {
          const int dx = box_x0 + r - x, dy = y - (box_y1 - 1 - r); inside = dx*dx + dy*dy <= r*r;
        } else if (!has_role && r > 0 && x >= box_x1 - r && y >= box_y1 - r) {
          const int dx = x - (box_x1 - 1 - r), dy = y - (box_y1 - 1 - r); inside = dx*dx + dy*dy <= r*r;
        }
        const int idx = (y * w + x) * 4;
        std::uint8_t* pp = px + idx;
        // 未绘制区（alpha=0 且 RGB 全 0）保持透明：杜绝"透明黑被后处理强制成
        // 不透明黑线"（真机根因 2026-10-06：候选窗左/上边缘 1px 黑边；
        // GDI/GDI+ 实际绘制过的像素必带内容 RGB≠0 或 alpha≠0，仍会强制不透明）
        if (pp[0] == 0 && pp[1] == 0 && pp[2] == 0 && pp[3] == 0) continue;
        if (inside) { px[idx + 3] = 0xFF; }
        else { px[idx] = px[idx+1] = px[idx+2] = px[idx+3] = 0; }
      }
    }
  }

  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  POINT pt{pos_x_, pos_y_};
  POINT pt_src{0, 0};
  SIZE win_size{size.cx, size.cy};
  const BOOL ulw_ok = UpdateLayeredWindow(hwnd_, nullptr, &pt, &win_size, mem, &pt_src, 0,
                                          &blend, ULW_ALPHA);
  // 防御日志：UpdateLayeredWindow 失败（size 非法/DC 异常）时窗口会隐形不弹
  ::ime::LogF( "[candidate] RenderToLayeredWindow ulw=%d size=%dx%d\n",
              static_cast<int>(ulw_ok != 0), size.cx, size.cy);
  // 注意：此处不再 SetWindowPos。窗口创建时已带 WS_EX_TOPMOST（EnsureWindow），
  // 位置由 UpdateLayeredWindow 的 pt 参数直接定位（分层窗口位置跟随 pt）。
  // 每次渲染都 SetWindowPos(HWND_TOPMOST) 会触发 TSF 布局变化 → OnLayoutChange
  // → DLL 上报位置 → SetPosition → 又渲染 → 死循环（真机 2026-10-07：服务
  // 假死、日志 6.6MB 刷屏、前台应用未响应）。对齐官方 WeaselPanel：渲染只
  // UpdateLayeredWindow，定位只在 SetPosition（位置变化时）做一次。

  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
}

// ---- role 角色图独立浮层（2026-10-04 真机需求）----
// role 图不画在候选窗窗口矩形内（会被矩形裁切压扁、被 preedit 白块挡住），
// 改为独立分层窗口：WS_EX_TRANSPARENT 鼠标穿透（纯展示，点击落到候选窗/下层），
// Z-order 永远在候选窗之上。固定高度 160px（宽按图片宽高比），锚候选窗右上角：
// 左部压在候选窗上、右部与上部溢出到候选窗外（对齐用户目标效果图）。
void CandidateWindow::EnsureRoleWindow() {
  if (role_hwnd_ != nullptr) return;
  static bool role_registered = false;
  if (!role_registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = RoleWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kRoleWindowClass;
    if (RegisterClassExW(&wc) != 0) role_registered = true;
  }
  role_hwnd_ = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,  // 鼠标穿透：整个窗口纯展示，不接收点击
      kRoleWindowClass, L"RimeBeautifyRole", WS_POPUP, 0, 0, 1, 1,
      nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

void CandidateWindow::ShowRoleLayer() {
  if (theme_.skin.mode != skin::SkinMode::Role || theme_.skin.role_image.empty()) {
    ::ime::LogF( "[role] skip mode=%d empty=%d\n",
                 static_cast<int>(theme_.skin.mode), theme_.skin.role_image.empty());
    HideRoleLayer(); return;
  }
  EnsureRoleWindow();
  if (role_hwnd_ == nullptr) {
    ::ime::LogF( "[role] FAIL hwnd null\n");
    return;
  }
  const std::wstring wpath = ResolveSkinPath(theme_.skin.role_image, config_dir_);
  ::ime::LogF( "[role] wpath=%ls\n", wpath.c_str());
  if (wpath.empty()) { HideRoleLayer(); return; }
  Gdiplus::Bitmap* bmp = GetCachedSkinBitmap(wpath, g_cache_role);
  if (bmp == nullptr) {
    ::ime::LogF( "[role] FAIL bmp load\n");
    HideRoleLayer(); return;
  }
  ::ime::LogF( "[role] OK %dx%d pos=(%d,%d) size=%dx%d\n",
               bmp->GetWidth(), bmp->GetHeight(), pos_x_, pos_y_,
               size_cache_.cx, size_cache_.cy);

  // 固定高度 160px（用户：尺寸你定，之后可调），宽度按图片宽高比
  const int role_h = theme_.skin.role_height;
  const int role_w = std::max(1, static_cast<int>(
      static_cast<std::int64_t>(bmp->GetWidth()) * role_h / bmp->GetHeight()));

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = role_w;
  bi.bmiHeader.biHeight = -role_h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP rbmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (rbmp == nullptr) {
    DeleteDC(mem); ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ old = SelectObject(mem, rbmp);
  {
    Gdiplus::Graphics g(mem);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));  // 透明底
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.DrawImage(bmp, Gdiplus::Rect(0, 0, role_w, role_h),
                0, 0, bmp->GetWidth(), bmp->GetHeight(), Gdiplus::UnitPixel);
  }
  SelectObject(mem, old);

  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  // 锚候选窗右上角：左部 35% 压候选窗、右部和上部溢出
  // 靠近屏幕右边缘时改为向左溢出，防止跑出屏幕
  int cx = pos_x_ + size_cache_.cx - static_cast<int>(role_w * 0.35);
  int screen_w = GetSystemMetrics(SM_CXSCREEN);
  if (cx + role_w > screen_w - 10) {
    // 向左溢出：锚候选窗左上角，右部 35% 压候选窗
    cx = pos_x_ - static_cast<int>(role_w * 0.65);
  }
  const int cy = pos_y_ - static_cast<int>(role_h * 0.25);
  POINT pt_dst{cx, cy};
  POINT pt_src{0, 0};
  SIZE sz{role_w, role_h};
  BOOL ulw = UpdateLayeredWindow(role_hwnd_, nullptr, &pt_dst, &sz, mem, &pt_src, 0, &blend, ULW_ALPHA);
  DWORD err = GetLastError();
  ::ime::LogF( "[role] UpdateLayeredWindow ret=%d err=%lu hwnd=%p pos=(%d,%d) size=%dx%d\n",
               static_cast<int>(ulw), err, role_hwnd_, cx, cy, role_w, role_h);
  // Z-order 压在候选窗之上
  SetWindowPos(role_hwnd_, HWND_TOPMOST, cx, cy, role_w, role_h,
               SWP_SHOWWINDOW | SWP_NOACTIVATE);
  ShowWindow(role_hwnd_, SW_SHOWNOACTIVATE);

  SelectObject(mem, old);
  DeleteObject(rbmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

void CandidateWindow::HideRoleLayer() {
  if (role_hwnd_ != nullptr) {
    ShowWindow(role_hwnd_, SW_HIDE);
  }
}

// ---- role 角色图大小拖拽调整（2026-10-04）----
void CandidateWindow::HandleRoleDoubleClick(int x, int y) {
  if (theme_.skin.mode != skin::SkinMode::Role || theme_.skin.role_image.empty()) return;
  // 算角色图区域
  const std::wstring rp = ResolveSkinPath(theme_.skin.role_image, config_dir_);
  if (rp.empty()) return;
  Gdiplus::Bitmap* rb = GetCachedSkinBitmap(rp, g_cache_role);
  if (rb == nullptr) return;
  const int rh = theme_.skin.role_height;
  const int rw = (int)((int64_t)rb->GetWidth() * rh / rb->GetHeight());
  // 角色图位置：x = size.cx - rw, y = 0
  SIZE size = size_cache_;
  const int role_x = size.cx - rw;
  const int role_y = 0;
  // 双击在角色图区域内？
  if (x >= role_x && x < role_x + rw && y >= role_y && y < role_y + rh) {
    role_edit_mode_ = !role_edit_mode_;  // 切换编辑模式
    if (role_edit_mode_) {
      role_drag_start_ = {x, y};
      role_drag_start_height_ = theme_.skin.role_height;
      role_drag_start_offset_x_ = role_offset_x_;
    }
    RenderToLayeredWindow();
  }
}

void CandidateWindow::HandleRoleMouseMove(int x, int y, bool left_down) {
  if (!role_edit_mode_ || theme_.skin.role_image.empty()) return;
  if (!left_down) return;  // 没按住左键
  // 垂直方向：调整高度
  const int dy = y - role_drag_start_.y;
  int new_h = role_drag_start_height_ + dy;
  if (new_h < 60) new_h = 60;   // 最小 60px
  if (new_h > 300) new_h = 300; // 最大 300px
  // 水平方向：调整 x 偏移（限制 -100 ~ +150）
  const int dx = x - role_drag_start_.x;
  int new_offset_x = role_drag_start_offset_x_ + dx;
  if (new_offset_x < -100) new_offset_x = -100;
  if (new_offset_x > 150) new_offset_x = 150;
  bool changed = false;
  if (new_h != theme_.skin.role_height) {
    theme_.skin.role_height = new_h;
    // 同步缩放快照：候选框/窗口几何（Measure/RenderLines 用 scaled_theme_）
    // 必须跟随拖拽，否则候选框不随月亮实时调整（根因 2026-10-13：
    // scaled_theme_ 为 SetTheme 时快照，拖拽只改 theme_ 导致候选框/窗口不动）
    scaled_theme_.skin.role_height = new_h;
    changed = true;
  }
  if (new_offset_x != role_offset_x_) {
    role_offset_x_ = new_offset_x;
    changed = true;
  }
  if (changed) {
    RenderToLayeredWindow();
  }
}

void CandidateWindow::HandleRoleKeyDown(WPARAM key) {
  if (!role_edit_mode_) return;
  if (key == VK_RETURN) {
    // 回车保存到 theme.yaml
    role_edit_mode_ = false;
    role_dragging_ = false;
    const std::string yaml_path = config_dir_ + "/theme.yaml";
    WriteThemeToYaml(yaml_path, theme_);
    RenderToLayeredWindow();
  } else if (key == VK_ESCAPE) {
    // ESC 取消
    role_edit_mode_ = false;
    role_dragging_ = false;
    RenderToLayeredWindow();
  }
}

}  // namespace ime::ui
