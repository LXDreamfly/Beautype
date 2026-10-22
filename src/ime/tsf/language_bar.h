// 语言栏中/英按钮（对齐小狼毫 WeaselTSF/LanguageBar.cpp 的 CLangBarItemButton）。
//
// 在系统语言栏（输入法指示器）显示 Beautype 图标按钮：中文态蓝底“中”、
// 英文态灰底“A”；左键单击切换中/英（ascii_mode），右键弹出菜单。
// text_service 在 ActivateEx 时 AddItem 注册、Deactivate 时 RemoveItem 注销；
// ascii_mode 变化时经 UpdateAsciiMode 通知语言栏刷新图标。
#pragma once

#include <windows.h>
#include <ctfutb.h>
#include <functional>

namespace ime {
namespace tsf {

class LanguageBarButton final : public ITfLangBarItemButton,
                                public ITfSource {
 public:
  // guid：语言栏项 GUID（用系统预定义 GUID_LBI_INPUTMODE 或自定义）；
  // clsid：本输入法 CLSID；thread_mgr：TSF 线程管理器（借用指针，text_service
  // 生命周期内有效，不 AddRef；右键菜单 owner 用它取焦点上下文窗口，对齐官方）；
  // ascii_toggle_cb：左键切换回调（text_service 设置，参数为切换后的目标状态）；
  // tray_command_cb：菜单命令回调（对齐官方语言栏右键完整菜单 → 命令经
  // TrayCommand IPC 发服务端执行，ID 见 kLangBar*）；open_pet_cb：右键菜单
  // 「打开桌宠」回调（本进程直接拉起 pet_preview.exe，不依赖 server 运行状态）。
  LanguageBarButton(REFGUID guid, REFCLSID clsid, ITfThreadMgr* thread_mgr,
                    std::function<void(bool)> ascii_toggle_cb,
                    std::function<void(UINT)> tray_command_cb,
                    std::function<void()> open_pet_cb);
  virtual ~LanguageBarButton();

  // ascii_mode 变化时调用：更新本地状态并通知语言栏刷新图标
  void UpdateAsciiMode(bool ascii_mode);

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // ITfLangBarItem
  STDMETHODIMP GetInfo(TF_LANGBARITEMINFO* pInfo) override;
  STDMETHODIMP GetStatus(DWORD* pdwStatus) override;
  STDMETHODIMP Show(BOOL fShow) override;
  STDMETHODIMP GetTooltipString(BSTR* pbstrToolTip) override;

  // ITfLangBarItemButton
  STDMETHODIMP OnClick(TfLBIClick click, POINT pt, const RECT* prcArea) override;
  STDMETHODIMP InitMenu(ITfMenu* pMenu) override;
  STDMETHODIMP OnMenuSelect(UINT wID) override;
  STDMETHODIMP GetIcon(HICON* phIcon) override;
  STDMETHODIMP GetText(BSTR* pbstrText) override;

  // ITfSource（语言栏 sink：OnUpdate 通知刷新）
  STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) override;
  STDMETHODIMP UnadviseSink(DWORD dwCookie) override;

 private:
  // 语言栏右键菜单命令 ID（对齐 server_main.cpp 的托盘 IDM_*：1001 中英切换 /
  // 1003 设置 / 1004 重新部署 / 1005 词典管理 / 1006 同步 / 1007 用户目录 /
  // 1008 日志目录 / 1009 退出；1010 打开桌宠为本进程处理，不占用服务端 IDM）。
  // 命令经 TrayCommand IPC 到服务端 AddMenuHandler。
  enum {
    kLangBarAsciiToggle = 1001,
    kLangBarSettings = 1003,
    kLangBarDeploy = 1004,
    kLangBarDict = 1005,
    kLangBarSync = 1006,
    kLangBarUserConfig = 1007,
    kLangBarLogDir = 1008,
    kLangBarExit = 1009,
    kLangBarOpenPet = 1010,
    kLangBarPetTop = 1011,  // 置顶桌宠（经 TrayCommand IPC → server IDM_PET_TOP_LANGBAR）
    kLangBarCheckUpdate = 1012,  // 检查更新（经 TrayCommand IPC → server IDM_CHECK_UPDATE）
  };
  static const UINT kMenuToggleAscii = 1;
  void NotifyUpdate(DWORD dwFlags);
  HMENU BuildPopupMenu();
  void HandleMenuCommand(UINT cmd);
  // 对齐官方 WeaselTSF::_GetFocusedContextWindow：取 TSF 焦点上下文所属窗口
  // （同线程），用作右键菜单 owner——修复 WPS 等多线程宿主下 GetForegroundWindow
  // 跨线程 owner 导致菜单不显示的老问题。
  HWND GetFocusedContextWindow();

  volatile ULONG ref_;
  GUID guid_;
  CLSID clsid_;
  ITfThreadMgr* thread_mgr_;  // 借用指针（text_service 生命周期内有效，不 AddRef）
  DWORD status_;
  DWORD sink_cookie_;
  ITfLangBarItemSink* sink_;
  bool ascii_mode_;
  std::function<void(bool)> ascii_toggle_cb_;
  std::function<void(UINT)> tray_command_cb_;
  std::function<void()> open_pet_cb_;
};

}  // namespace tsf
}  // namespace ime
