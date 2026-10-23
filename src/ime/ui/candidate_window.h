// 候选窗（M1）：只消费 skin::Theme 渲染候选，不感知皮肤来源。
// CI 可验证的部分是纯逻辑/GDI 离屏路径（Measure / RenderToMemoryDib）；
// 交互会话路径（Show/Hide 的分层窗口）在真机上验收（M1.1）。
#pragma once

// windows.h 的 min/max 宏会破坏 std::min/std::max（GDI+ 圆角计算用到 std::min）。
// 必须在 include <windows.h> **之前**定义 NOMINMAX（若放 .cpp 里，本头文件
// 先于宏定义引入 windows.h，宏已展开，定义无效——C2589）。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include "skin/theme.h"

namespace ime::ui {

// 九宫格（9-patch）单段：源矩形（从原图切出）→ 目标矩形（画到候选窗）。
// sx/sy/sw/sh 为原图坐标，dx/dy/dw/dh 为目标窗口坐标（docs/skin-format.md §3）。
struct NinePatchRect {
  int sx = 0, sy = 0, sw = 0, sh = 0;
  int dx = 0, dy = 0, dw = 0, dh = 0;
};
constexpr int kNinePatchCount = 9;

// 计算 9 段源/目标矩形（可无头测试的纯函数）。顺序：
// 0 左上角 1 上边 2 右上角 3 左边 4 中间 5 右边 6 左下角 7 下边 8 右下角。
// 不变形保证：四角 dw==sw 且 dh==sh；上/下边 dh==sh；左/右边 dw==sw。
// 中间区（文字区）双向拉伸。尺寸不足时按最小 1px 夹紧。
void ComputeNinePatch(int img_w, int img_h, int dst_w, int dst_h,
                      const skin::SkinBorders& b, NinePatchRect out[kNinePatchCount]);

class CandidateWindow {
 public:
  CandidateWindow() = default;
  ~CandidateWindow();

  CandidateWindow(const CandidateWindow&) = delete;
  CandidateWindow& operator=(const CandidateWindow&) = delete;

  void SetTheme(const skin::Theme& theme);
  // caret_top = 光标行 top（DLL 上报 rc.top）：屏幕底部放不下时翻到光标上方
  // 需要它（对齐官方 _RepositionWindow 的 m_inputPos.top 语义）。
  void SetPosition(int x, int y, int caret_top = 0);

  // 月亮比候选框高出来的 Y 偏移（role_height - content_height）。
  // 用于窗口位置计算：把整个窗口往上移 offset，让候选框（在窗口底部）
  // 刚好落在光标下方，不被月亮推远。
  int GetRoleOffsetY() const;

  // 皮肤图片所在目录（theme.yaml 目录；PNG 皮肤字段存相对文件名，
  // 运行时用该目录解析绝对路径——真机根因 2026-10-03：工作目录 ≠ 配置目录，
  // 相对路径 Bitmap 加载全部失败、皮肤回退纯色/无预览）。
  void SetConfigDir(const std::string& config_dir);

  // 候选窗标题显示当前输入方案 id（如 luna_pinyin / double_pinyin / wubi86），
  // 方案切换后用户可直接看到当前用的是哪套方案。
  void SetSchemaTitle(const std::string& schema_id);

  // 组合区（preedit）文本：已选词 + 剩余拼音（librime composition.preedit）。
  // 对齐小狼毫 preedit_type: composition —— 点选候选后"已选中的词"显示在
  // 候选窗顶部组合区，未全部选完前不 commit 上屏，用户能看到已选词没有丢。
  // （真机根因 2026-10-04：点选部分候选后 EDIT 空白，用户以为字丢了——
  //   librime 语义是全部选完/按空格才 commit；weasel 靠 preedit 行给反馈。）
  void SetPreedit(const std::string& utf8) { preedit_ = utf8; }
  const std::string& preedit() const { return preedit_; }

  // 更新候选并显示（交互会话）；candidates 为当前页候选文本，highlighted 为高亮下标。
  void Show(const std::vector<std::string>& candidates, int highlighted);
  void Hide();

  // 主线程预创建窗口（对齐官方 weasel：WeaselServerApp.cpp:30 m_ui.Create() 在
  // server.Run() 之前于主线程创建 UI 窗口）。候选窗窗口必须属于消息泵线程，
  // 否则窗口创建在 pipe 工作线程（无 GetMessage 循环，阻塞在管道读取）→
  // 鼠标悬停/点击/滚轮消息无人处理 → 沙漏假死；且多宿主并发时 pipe 线程
  // 跨线程 ShowWindow/SetWindowPos → SendMessage 等一个正阻塞读管道的线程
  // → 死锁（真机根因 2026-10-07：悬停月亮 → 沙漏 → 输入框卡死）。
  void CreateMainWindow();

  // 离屏渲染验证工具（不创建窗口）：用当前主题把候选渲染成 PNG 文件，
  // 用于真机问题定位（文字截断/布局），避免在用户桌面擅自弹窗。
  bool RenderToPng(const std::string& utf8_path,
                   const std::vector<std::string>& candidates, int highlighted) const;

  // 用户交互回调（标准输入法候选窗语义）：点击选中候选 / 滚轮翻页。
  // 候选窗只发事件不持服务引用，由宿主（RimeTextService）驱动引擎动作。
  using ClickCallback = std::function<void(int index)>;
  using ScrollCallback = std::function<void(bool up)>;  // true=上滚(上一页) false=下滚(下一页)
  using RightClickCallback = std::function<void(void)>;  // 右键候选窗（弹输入法菜单）
  void SetClickCallback(ClickCallback cb) { click_cb_ = std::move(cb); }
  void SetScrollCallback(ScrollCallback cb) { scroll_cb_ = std::move(cb); }
  void SetRightClickCallback(RightClickCallback cb) { right_click_cb_ = std::move(cb); }

  // 窗口过程入口（自由函数 CandidateWindowProc 调用；内部做命中测试并触发回调）
  void HandleMouseClick(int x, int y) {
    const int idx = HitTestCandidate(x, y);
    if (idx >= 0 && click_cb_) {
      click_cb_(idx);
    }
  }
  void HandleMouseWheel(bool up) {
    if (scroll_cb_) {
      scroll_cb_(up);
    }
  }
  // 右键候选窗：触发输入法菜单回调（WndProc WM_RBUTTONUP 调用）
  void HandleRightClick() {
    if (right_click_cb_) {
      right_click_cb_();
    }
  }

  // role 角色图大小拖拽调整（窗口过程调用）
  void HandleRoleDoubleClick(int x, int y);
  void HandleRoleMouseMove(int x, int y, bool left_down);
  void HandleRoleKeyDown(WPARAM key);

  // 纯逻辑测量：给定候选列表计算窗口尺寸（不依赖 HWND；GDI 文本测量可无头运行）。
  // config_dir 用于解析皮肤图片相对路径（PNG 皮肤模式；纯色模式可省略）。
  // preedit 非空时顶部多一行组合区（已选词+剩余拼音），窗口高度相应增加。
  static SIZE Measure(const skin::Theme& theme,
                      const std::vector<std::string>& candidates,
                      const std::string& config_dir = {},
                      const std::string& preedit = {});

  // 离屏渲染冒烟：按主题把 candidates 渲染到 32bpp BGRA 内存位图（argb 需
  // width*height*4 字节）；返回是否产生非背景像素（证明文本真的画出来了）。
  // 不依赖 HWND，可无头运行。
  bool RenderToMemoryDib(const std::vector<std::string>& candidates, int highlighted,
                         int width, int height, std::uint8_t* argb) const;

 private:
  void EnsureWindow();
  void RenderToLayeredWindow();
  bool RenderLines(HDC dc, int width, int height,
                   const std::vector<std::string>& candidates, int highlighted) const;
  // 命中测试：窗口坐标 → 候选下标（-1 = 未命中候选文字区；与 RenderLines 同布局）
  int HitTestCandidate(int x, int y) const;

  // ---- role 角色图独立浮层（2026-10-04 真机需求：月亮图比候选窗大、溢出右侧、
  // 压在候选窗最上层、鼠标不挡候选点击）----
  // role 图不再画在候选窗窗口矩形内（会被矩形裁切压扁），改为独立分层窗口：
  // WS_EX_TRANSPARENT 鼠标穿透（纯展示，点击落到候选窗/下层），Z-order 永远在
  // 候选窗之上。固定高度 160px（宽按图片宽高比），锚候选窗右上角：左部压在
  // 候选窗上、右部与上部溢出到候选窗外（对齐用户目标效果图）。
  void EnsureRoleWindow();
  void ShowRoleLayer();   // 按当前 theme/config_dir 渲染 role 图并显示/定位
  void HideRoleLayer();    // 随候选窗一起隐藏

  skin::Theme theme_;
  skin::Theme scaled_theme_;  // DPI 缩放后的主题副本（用于渲染，不污染用户配置）
  float dpi_scale_ = 1.0f;     // DPI 缩放系数（dpi / 96.0）
  bool dpi_inited_ = false;    // DPI 是否已初始化（只获取一次）
  std::string config_dir_;  // 皮肤图片目录（相对文件名 → 绝对路径解析用）
  std::vector<std::string> candidates_;
  std::string preedit_;     // 组合区文本（已选词+剩余拼音；空 = 不显示组合区）
  int highlighted_ = -1;
  ClickCallback click_cb_;
  ScrollCallback scroll_cb_;
  RightClickCallback right_click_cb_;
  int pos_x_ = 0;
  int pos_y_ = 0;
  // 是否已收到有效窗口位置（服务端 UpdateInputPosition / DLL 位置上报）。
  // 初始为 false：位置未就绪前 Show 只挂起不弹窗，避免候选窗以 (0,0)
  // 在左上角闪现（真机根因 2026-10-07）。
  bool has_position_ = false;
  // 位置未就绪时挂起的 Show 请求：SetPosition 到位后补弹。
  bool pending_show_ = false;
  // 屏幕底部"翻到光标上方"记忆（对齐官方 WeaselPanel::_RepositionWindow
  // 的 m_sticky，WeaselPanel.cpp:1222-1292）：候选窗在屏幕底部被翻到光标
  // 上方后记住，内容/位置微小变化不弹回下方（防弹跳）；位置大幅变化
  // （>50px）或候选清空时重置。
  bool sticky_ = false;
  SIZE size_cache_{0, 0};  // 最近一次测量尺寸（防御日志用）
  HWND hwnd_ = nullptr;
  HWND role_hwnd_ = nullptr;  // role 角色图独立浮层窗口
  static const wchar_t kWindowClass[];
  static const wchar_t kRoleWindowClass[];

  // ---- role 角色图大小拖拽调整（2026-10-04）----
  bool role_edit_mode_ = false;    // 是否在编辑模式
  bool role_dragging_ = false;     // 是否正在拖拽
  POINT role_drag_start_{0,0};     // 拖拽起点
  int role_drag_start_height_ = 0; // 拖拽开始时的角色图高度
  int role_offset_x_ = 0;          // 角色图 x 轴偏移（用户左右拖拽调整）
  int role_drag_start_offset_x_ = 0; // 拖拽开始时的 x 偏移
};

}  // namespace ime::ui
