// 见 language_bar.h 顶部注释。实现对齐小狼毫 CLangBarItemButton 语义。
#include "tsf/language_bar.h"

#include <objbase.h>
#include <string>
#include <olectl.h>  // CONNECT_E_*（AdviseSink 错误码）

// DLL 模块句柄（定义于 text_service.cpp，DllMain 赋值；对齐官方
// WeaselTSF/Globals.h 的 extern g_hInst）。部署改名 beautype_tsf.dll 后
// GetModuleHandleW(L"rime_tsf.dll") 返回空 → 语言栏图标加载失败
// （真机：蓝底白问号）；一律用 g_hinst 取本 DLL 内嵌资源。
extern HMODULE g_hinst;

namespace ime {
namespace tsf {

namespace {

// 本输入法 CLSID（与 tsf_dll.cpp / register_tip.cpp 同值，内部链接避免重复定义）
const CLSID kClsidBeautypeTSF = {
    0xB17E78C8, 0xABAF, 0x4891, {0x84, 0x41, 0x42, 0xFC, 0x45, 0xF4, 0x75, 0xA4}};

// 程序化绘制 16x16 语言栏图标（中文态“中”/英文态“A”），避免依赖外部图标文件
HICON MakeTextIcon(const wchar_t* text, COLORREF bg, COLORREF fg) {
  const int kSize = 16;
  HDC screen = GetDC(nullptr);
  if (screen == nullptr) {
    return nullptr;
  }
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, kSize, kSize);
  if (mem == nullptr || bmp == nullptr) {
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return nullptr;
  }
  HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem, bmp));
  RECT rc = {0, 0, kSize, kSize};
  HBRUSH brush = CreateSolidBrush(bg);
  FillRect(mem, &rc, brush);
  DeleteObject(brush);
  SetBkMode(mem, TRANSPARENT);
  SetTextColor(mem, fg);
  HFONT font = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
  HFONT old_font = static_cast<HFONT>(SelectObject(mem, font));
  RECT tr = {0, 1, kSize, kSize + 1};
  DrawTextW(mem, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  SelectObject(mem, old_font);
  DeleteObject(font);
  SelectObject(mem, old_bmp);

  ICONINFO ii = {};
  ii.fIcon = TRUE;
  ii.hbmColor = bmp;
  // 全 0 掩码 = 图标全部不透明
  ii.hbmMask = CreateBitmap(kSize, kSize, 1, 1, nullptr);
  HICON icon = (ii.hbmMask != nullptr) ? CreateIconIndirect(&ii) : nullptr;
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return icon;
}

// 语言栏图标：DLL 资源内嵌 Beautype 蝴蝶 logo（langbar-zh.ico / langbar-en.ico）。
// v4.7.5 对齐官方 WeaselTSF/LanguageBar.cpp:198-213 的 GetIcon 主路径——
// g_hinst（DLL 实例句柄）+ LoadImageW(MAKEINTRESOURCEW, IMAGE_ICON,
// SM_CXSMICON, SM_CYSMICON, LR_SHARED)。修复前用
// GetModuleHandleW(L"rime_tsf.dll") 按模块名找句柄，部署改名 beautype_tsf.dll
// 后返回空 → 图标加载失败（蓝底白问号）。注：官方另有 current_ascii_icon /
// current_zhung_icon 配置覆盖分支，Beautype 无此配置项，不新增（避免越修越多）。
HICON ChineseIcon() {
  static HICON icon = static_cast<HICON>(::LoadImageW(
      g_hinst, MAKEINTRESOURCEW(102),  // IDI_LANGBAR_ZH
      IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
      LR_SHARED));
  return icon;
}
HICON EnglishIcon() {
  static HICON icon = static_cast<HICON>(::LoadImageW(
      g_hinst, MAKEINTRESOURCEW(103),  // IDI_LANGBAR_EN
      IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
      LR_SHARED));
  return icon;
}

const DWORD kLangBarItemSinkCookie = 0x4242BEAU;

}  // namespace

LanguageBarButton::LanguageBarButton(REFGUID guid, REFCLSID clsid,
                                     ITfThreadMgr* thread_mgr,
                                     std::function<void(bool)> ascii_toggle_cb,
                                     std::function<void(UINT)> tray_command_cb,
                                     std::function<void()> open_pet_cb)
    : ref_(1),
      guid_(guid),
      clsid_(clsid),
      thread_mgr_(thread_mgr),
      status_(0),
      sink_cookie_(0),
      sink_(nullptr),
      ascii_mode_(false),
      ascii_toggle_cb_(std::move(ascii_toggle_cb)),
      tray_command_cb_(std::move(tray_command_cb)),
      open_pet_cb_(std::move(open_pet_cb)) {}

LanguageBarButton::~LanguageBarButton() {
  if (sink_ != nullptr) {
    sink_->Release();
    sink_ = nullptr;
  }
}

void LanguageBarButton::UpdateAsciiMode(bool ascii_mode) {
  if (ascii_mode_ == ascii_mode) {
    return;
  }
  ascii_mode_ = ascii_mode;
  NotifyUpdate(TF_LBI_ICON | TF_LBI_STATUS);
}

STDMETHODIMP LanguageBarButton::QueryInterface(REFIID riid, void** ppv) {
  if (ppv == nullptr) {
    return E_INVALIDARG;
  }
  *ppv = nullptr;
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfLangBarItem) ||
      IsEqualIID(riid, IID_ITfLangBarItemButton)) {
    *ppv = static_cast<ITfLangBarItemButton*>(this);
  } else if (IsEqualIID(riid, IID_ITfSource)) {
    *ppv = static_cast<ITfSource*>(this);
  }
  if (*ppv != nullptr) {
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) LanguageBarButton::AddRef() { return ++ref_; }

STDMETHODIMP_(ULONG) LanguageBarButton::Release() {
  const ULONG n = --ref_;
  if (n == 0) {
    delete this;
  }
  return n;
}

STDMETHODIMP LanguageBarButton::GetInfo(TF_LANGBARITEMINFO* pInfo) {
  if (pInfo == nullptr) {
    return E_INVALIDARG;
  }
  pInfo->clsidService = clsid_;
  pInfo->guidItem = guid_;
  pInfo->dwStyle = TF_LBI_STYLE_BTN_BUTTON | TF_LBI_STYLE_BTN_MENU |
                   TF_LBI_STYLE_SHOWNINTRAY;
  pInfo->ulSort = 1;
  lstrcpyW(pInfo->szDescription, L"Beautype 输入法");
  return S_OK;
}

STDMETHODIMP LanguageBarButton::GetStatus(DWORD* pdwStatus) {
  if (pdwStatus == nullptr) {
    return E_INVALIDARG;
  }
  *pdwStatus = status_;
  return S_OK;
}

STDMETHODIMP LanguageBarButton::Show(BOOL fShow) {
  const DWORD hidden = TF_LBI_STATUS_HIDDEN;
  if (fShow) {
    status_ &= ~hidden;
  } else {
    status_ |= hidden;
  }
  NotifyUpdate(TF_LBI_STATUS);
  return S_OK;
}

STDMETHODIMP LanguageBarButton::GetTooltipString(BSTR* pbstrToolTip) {
  if (pbstrToolTip == nullptr) {
    return E_INVALIDARG;
  }
  *pbstrToolTip = SysAllocString(ascii_mode_ ? L"Beautype：英文模式（点击切换中文）"
                                             : L"Beautype：中文模式（点击切换英文）");
  return (*pbstrToolTip != nullptr) ? S_OK : E_OUTOFMEMORY;
}

// 构建语言栏右键/展开菜单（对齐官方 IDR_MENU_POPUP 的可用功能项；
// 帮助/论坛/更新等 weasel 专属项对 Beautype 无意义，不引入）
HMENU LanguageBarButton::BuildPopupMenu() {
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return nullptr;
  }
  AppendMenuW(menu, MF_STRING, kLangBarAsciiToggle,
              ascii_mode_ ? L"切换到中文输入" : L"切换到英文输入");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kLangBarSettings, L"自定义皮肤");
  AppendMenuW(menu, MF_STRING, kLangBarDict, L"词典管理");
  AppendMenuW(menu, MF_STRING, kLangBarSync, L"用户数据同步");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kLangBarOpenPet, L"打开桌宠");
  AppendMenuW(menu, MF_STRING, kLangBarPetTop, L"置顶桌宠");
  AppendMenuW(menu, MF_STRING, kLangBarUserConfig, L"打开用户目录");
  AppendMenuW(menu, MF_STRING, kLangBarLogDir, L"打开日志目录");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kLangBarDeploy, L"重新部署");
  AppendMenuW(menu, MF_STRING, kLangBarCheckUpdate, L"检查更新");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kLangBarExit, L"退出");
  return menu;
}

void LanguageBarButton::HandleMenuCommand(UINT cmd) {
  if (cmd == kLangBarAsciiToggle) {
    if (ascii_toggle_cb_) {
      ascii_toggle_cb_(!ascii_mode_);
    }
  } else if (cmd == kLangBarOpenPet) {
    // 「打开桌宠」：本进程直接拉起（不依赖 server 运行状态；路径经
    // text_service LaunchPetFromLangBar 读注册表 InstallDir → pet\pet_preview.exe）
    if (open_pet_cb_) {
      open_pet_cb_();
    }
  } else if (cmd != 0 && tray_command_cb_) {
    // 其余菜单项：命令 ID 与服务端托盘 IDM_* 一致，经 TrayCommand IPC 执行
    tray_command_cb_(cmd);
  }
}

STDMETHODIMP LanguageBarButton::OnClick(TfLBIClick click, POINT pt,
                                        const RECT*) {
  if (click == TF_LBI_CLK_LEFT) {
    if (ascii_toggle_cb_) {
      ascii_toggle_cb_(!ascii_mode_);
    }
  } else if (click == TF_LBI_CLK_RIGHT) {
    // 右键：对齐官方 IDR_MENU_POPUP 完整菜单（中英切换/设置/词典/同步/
    // 目录/部署/退出）；命令经 TrayCommand IPC 发服务端 AddMenuHandler 执行。
    // 菜单位置用点击坐标 pt（官方 TrackPopupMenuEx 用 pt.x/pt.y），
    // 修复前传 (0,0) → 菜单弹在屏幕左上角（真机 2026-10-07）。
    // owner 用 TSF 焦点上下文窗口（GetFocusedContextWindow，同线程）而非
    // GetForegroundWindow：WPS 等多线程宿主下前景窗口可能属其他线程，
    // TrackPopupMenu 跨线程 owner 会导致菜单不显示（老问题「WPS 里右键
    // 语言栏打不开菜单」根因，对齐官方 WeaselTSF/LanguageBar.cpp OnClick）。
    HWND hwnd = GetFocusedContextWindow();
    if (hwnd == nullptr) {
      return S_OK;  // 对齐官方：拿不到焦点上下文窗口则不弹
    }
    HMENU menu = BuildPopupMenu();
    if (menu != nullptr) {
      const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY |
                                                TPM_RIGHTBUTTON,
                                      pt.x, pt.y, 0, hwnd, nullptr);
      DestroyMenu(menu);
      HandleMenuCommand(cmd);
    }
  }
  return S_OK;
}

// 对齐官方 WeaselTSF::_GetFocusedContextWindow（WeaselTSF/LanguageBar.cpp:341）：
// ThreadMgr→GetFocus(docMgr)→GetTop(context)→GetActiveView(view)→GetWnd。
// 返回当前聚焦的 TSF 文档上下文所属窗口（与本语言栏按钮同线程）。
HWND LanguageBarButton::GetFocusedContextWindow() {
  HWND hwnd = nullptr;
  if (thread_mgr_ == nullptr) {
    return hwnd;
  }
  ITfDocumentMgr* doc_mgr = nullptr;
  if (SUCCEEDED(thread_mgr_->GetFocus(&doc_mgr)) && doc_mgr != nullptr) {
    ITfContext* context = nullptr;
    if (SUCCEEDED(doc_mgr->GetTop(&context)) && context != nullptr) {
      ITfContextView* view = nullptr;
      if (SUCCEEDED(context->GetActiveView(&view)) && view != nullptr) {
        view->GetWnd(&hwnd);
        view->Release();
      }
      context->Release();
    }
    doc_mgr->Release();
  }
  return hwnd;
}

STDMETHODIMP LanguageBarButton::InitMenu(ITfMenu* pMenu) {
  if (pMenu == nullptr) {
    return E_INVALIDARG;
  }
  // 系统语言栏展开菜单：同右键完整菜单（对齐官方 IDR_MENU_POPUP）
  const wchar_t* toggle = ascii_mode_ ? L"切换到中文输入" : L"切换到英文输入";
  pMenu->AddMenuItem(kLangBarAsciiToggle, 0, nullptr, nullptr, toggle,
                     static_cast<ULONG>(wcslen(toggle)), nullptr);
  pMenu->AddMenuItem(0, TF_LBMENUF_SEPARATOR, nullptr, nullptr, nullptr, 0,
                     nullptr);
  const wchar_t* settings = L"自定义皮肤";
  pMenu->AddMenuItem(kLangBarSettings, 0, nullptr, nullptr, settings,
                     static_cast<ULONG>(wcslen(settings)), nullptr);
  const wchar_t* dict = L"词典管理";
  pMenu->AddMenuItem(kLangBarDict, 0, nullptr, nullptr, dict,
                     static_cast<ULONG>(wcslen(dict)), nullptr);
  const wchar_t* sync = L"用户数据同步";
  pMenu->AddMenuItem(kLangBarSync, 0, nullptr, nullptr, sync,
                     static_cast<ULONG>(wcslen(sync)), nullptr);
  pMenu->AddMenuItem(0, TF_LBMENUF_SEPARATOR, nullptr, nullptr, nullptr, 0,
                     nullptr);
  const wchar_t* open_pet = L"打开桌宠";
  pMenu->AddMenuItem(kLangBarOpenPet, 0, nullptr, nullptr, open_pet,
                     static_cast<ULONG>(wcslen(open_pet)), nullptr);
  const wchar_t* pet_top = L"置顶桌宠";
  pMenu->AddMenuItem(kLangBarPetTop, 0, nullptr, nullptr, pet_top,
                     static_cast<ULONG>(wcslen(pet_top)), nullptr);
  const wchar_t* user = L"打开用户目录";
  pMenu->AddMenuItem(kLangBarUserConfig, 0, nullptr, nullptr, user,
                     static_cast<ULONG>(wcslen(user)), nullptr);
  const wchar_t* logdir = L"打开日志目录";
  pMenu->AddMenuItem(kLangBarLogDir, 0, nullptr, nullptr, logdir,
                     static_cast<ULONG>(wcslen(logdir)), nullptr);
  pMenu->AddMenuItem(0, TF_LBMENUF_SEPARATOR, nullptr, nullptr, nullptr, 0,
                     nullptr);
  const wchar_t* deploy = L"重新部署";
  pMenu->AddMenuItem(kLangBarDeploy, 0, nullptr, nullptr, deploy,
                     static_cast<ULONG>(wcslen(deploy)), nullptr);
  const wchar_t* check_update = L"检查更新";
  pMenu->AddMenuItem(kLangBarCheckUpdate, 0, nullptr, nullptr, check_update,
                     static_cast<ULONG>(wcslen(check_update)), nullptr);
  pMenu->AddMenuItem(0, TF_LBMENUF_SEPARATOR, nullptr, nullptr, nullptr, 0,
                     nullptr);
  const wchar_t* quit = L"退出";
  pMenu->AddMenuItem(kLangBarExit, 0, nullptr, nullptr, quit,
                     static_cast<ULONG>(wcslen(quit)), nullptr);
  return S_OK;
}

STDMETHODIMP LanguageBarButton::OnMenuSelect(UINT wID) {
  HandleMenuCommand(wID);
  return S_OK;
}

STDMETHODIMP LanguageBarButton::GetIcon(HICON* phIcon) {
  if (phIcon == nullptr) {
    return E_INVALIDARG;
  }
  *phIcon = ascii_mode_ ? EnglishIcon() : ChineseIcon();
  return (*phIcon != nullptr) ? S_OK : E_FAIL;
}

STDMETHODIMP LanguageBarButton::GetText(BSTR* pbstrText) {
  if (pbstrText == nullptr) {
    return E_INVALIDARG;
  }
  *pbstrText = SysAllocString(L"Beautype");
  return (*pbstrText != nullptr) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP LanguageBarButton::AdviseSink(REFIID riid, IUnknown* punk,
                                           DWORD* pdwCookie) {
  if (pdwCookie == nullptr) {
    return E_INVALIDARG;
  }
  if (!IsEqualIID(riid, IID_ITfLangBarItemSink)) {
    return CONNECT_E_CANNOTCONNECT;
  }
  if (sink_ != nullptr) {
    return CONNECT_E_ADVISELIMIT;
  }
  if (punk == nullptr ||
      FAILED(punk->QueryInterface(IID_ITfLangBarItemSink,
                                  reinterpret_cast<void**>(&sink_)))) {
    sink_ = nullptr;
    return E_NOINTERFACE;
  }
  *pdwCookie = kLangBarItemSinkCookie;
  return S_OK;
}

STDMETHODIMP LanguageBarButton::UnadviseSink(DWORD dwCookie) {
  if (dwCookie != kLangBarItemSinkCookie || sink_ == nullptr) {
    return CONNECT_E_NOCONNECTION;
  }
  sink_->Release();
  sink_ = nullptr;
  return S_OK;
}

void LanguageBarButton::NotifyUpdate(DWORD dwFlags) {
  if (sink_ != nullptr) {
    sink_->OnUpdate(dwFlags);
  }
}

}  // namespace tsf
}  // namespace ime
