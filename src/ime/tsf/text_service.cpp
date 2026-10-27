#include "text_service.h"

// tsf_dll.cpp 中 DEFINE_GUID 定义的同 DLL 文本服务 CLSID
extern "C" const GUID CLSID_BeautypeTSF;

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>
#include <oleauto.h>  // VariantClear / SysAllocString
#include <ctffunc.h>  // GUID_LBI_INPUTMODE
#include <shellapi.h>  // ShellExecuteW（拉起服务进程）
#include <tlhelp32.h>  // CreateToolhelp32Snapshot（服务进程计数，照官方 WeaselTSF.cpp:245）

// kKeyMask* 修饰键位常量（与 librime/weasel 一致：SHIFT=1<<0, LOCK=1<<1,
// CONTROL=1<<2, ALT=1<<3）。text_service.h 刻意不引 rime_engine.h（服务化后
// DLL 不加载 librime 引擎），此处仅取常量定义。
#include "engine/rime_engine.h"
#include "log/log.h"
#include "i18n/strings.h"

// 出厂默认桌宠皮肤文件名（统一约定，勿散落硬编码）。
// 以后想换默认动画：把新做好的 .riv 命名为 beautype_pet.riv 替换安装包里的文件即可，
// 无需改代码。改名时全局搜索 kDefaultRivName 同步（server/text_service/pet_preview/install.nsi）。
inline constexpr wchar_t kDefaultRivName[] = L"beautype_pet.riv";

// M1.1 最终形态（TSF DLL）：本服务不再依赖演示宿主的全局窗口。
// 点选候选直接走 TSF 文档插入（InsertCommitText）；候选窗随光标定位
// （ITfContextView::GetScreenExt，见 UpdateCandidateWindow）。

// DLL 模块句柄（tsf_dll.cpp DllMain 赋值）。定义在本文件（rime_frontend
// 静态库成员，rime_tsf 与 test_tsf 共用；tsf_dll.cpp 仅 extern 声明），
// 对齐官方 WeaselTSF/Globals.cpp 的 g_hInst。部署改名 beautype_tsf.dll 后
// GetModuleHandleW(L"rime_tsf.dll") 返回空，本 DLL 内取自身路径/资源
// 一律用此实例句柄。
HMODULE g_hinst = nullptr;

// 统一日志文件（g_log 由 rime_log 库定义，DLL 内为 rime_tsf.log；无 stdout）。
// 与 rime_engine.cpp 同款，保证 TSF 活动也落入日志便于本机排查。
// v4.7.6：定义提前到匿名 namespace 之前——拉起服务/安装目录读取等
// namespace 内逻辑也需要记录日志（观测闭环：服务被杀/拉起失败当场可见）。
#define LOG(fmt, ...)                                                          \
  do {                                                                         \
    if (::ime::g_log != nullptr) {                                             \
      ::ime::LogF( fmt, ##__VA_ARGS__);                          \
      std::fflush(::ime::g_log);  /* 常驻进程日志实时落盘（真机排查根因 2026-10-05） */ \
    }                                                                          \
  } while (0)

namespace {

// 宽字符 → UTF-8（服务端 commit/preedit 为宽字符，上屏/日志用 UTF-8）
std::string ToUtf8(const std::wstring& w) {
  if (w.empty()) {
    return {};
  }
  const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                      nullptr, nullptr);
  if (len <= 1) {
    return {};
  }
  std::string s(static_cast<std::size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], len, nullptr, nullptr);
  return s;
}

// 服务安装目录（对齐官方 WeaselTSF LanguageBar.cpp:289-293 _GetRootDir()：
// 官方从注册表读 WeaselRoot 定位安装根，再 ShellExecute start_service.bat）。
// Beautype 官方式部署 DLL 到 System32/SysWOW64（照官方 imesetup.cpp:158-163），
// DLL 所在目录 ≠ 安装目录；安装器已写 HKLM\Software\Beautype\InstallDir
// （install.nsi:42，NSIS 默认 32 位视图写入，实测键在 WOW6432Node 视图）。
// 两个 DLL 统一 KEY_WOW64_32KEY 读 32 位视图（官方 32 位 DLL 也是读
// WOW6432Node 字面路径，LanguageBar.cpp:37-42；本机 64 位视图无此键，
// 不照抄官方视图分支，避免 64 位 DLL 扑空）。
// v4.7.6：DLL 不再用自身目录拼服务路径——真机事故 2026-10-12：服务被外部
// 杀死后 DLL 拉起 system32\rime_server.exe（不存在）永远失败 → Beautype
// 成「选中但永久失效」的死输入法，输入栈卡死、打不了中文。
std::wstring RimeServerInstallDir() {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Beautype", 0,
                      KEY_READ | KEY_WOW64_32KEY, &key) != ERROR_SUCCESS) {
    return {};
  }
  wchar_t buf[MAX_PATH] = {};
  DWORD size = sizeof(buf);
  const LSTATUS r = ::RegGetValueW(key, nullptr, L"InstallDir", RRF_RT_REG_SZ,
                                   nullptr, buf, &size);
  ::RegCloseKey(key);
  if (r != ERROR_SUCCESS || buf[0] == L'\0') {
    return {};
  }
  std::wstring dir(buf);
  // 去尾部反斜杠/空白（NSIS 写 "$INSTDIR" 无尾部反斜杠；防御手工/旧版残留）
  while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L' ')) {
    dir.pop_back();
  }
  return dir;
}

// 当前运行的 rime_server.exe 进程数（照官方 WeaselTSF.cpp:245-260 进程计数）
int CountRimeServerProcesses() {
  int count = 0;
  const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return 0;
  }
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      // 新进程名 BeautypeServer.exe（对齐官方 WeaselServer.exe）；过渡期同时识别
      // 旧名 rime_server.exe，避免从旧版升级时旧进程常驻却被判"服务不在"而重复拉起。
      if (_wcsicmp(pe.szExeFile, L"BeautypeServer.exe") == 0 ||
          _wcsicmp(pe.szExeFile, L"rime_server.exe") == 0) {
        ++count;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return count;
}

// 拉起服务进程（对齐官方 WeaselTSF.cpp:263-269：官方 ShellExecute
// start_service.bat，路径来自注册表 WeaselRoot；我们直接 ShellExecute
// rime_server.exe，SW_HIDE 无窗口、异步返回）。注册表缺失/路径不存在时
// 记日志并跳过，不硬编码路径。
void LaunchRimeServer() {
  const std::wstring dir = RimeServerInstallDir();
  if (dir.empty()) {
    LOG("[tsf] 注册表无 Beautype InstallDir，跳过拉起服务进程\n");
    return;
  }
  // 新名 BeautypeServer.exe（对齐官方 WeaselServer.exe）；旧名 rime_server.exe
  // 仅作残留回退（安装器会清理旧实体，正常路径不会走到）。
  std::wstring exe = dir + L"\\BeautypeServer.exe";
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    exe = dir + L"\\rime_server.exe";
  }
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    LOG("[tsf] 拉起失败：%ls 不存在（安装目录异常）\n", exe.c_str());
    return;
  }
  const HINSTANCE hr = ::ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr,
                                       dir.c_str(), SW_HIDE);
  LOG("[tsf] 拉起 BeautypeServer.exe dir=%ls ret=%d\n", dir.c_str(),
      static_cast<int>(reinterpret_cast<intptr_t>(hr)));
}

// 语言栏右键「打开桌宠」（M5 便携轮）：读注册表 InstallDir → pet\pet_preview.exe
// + 默认皮肤 riv → ShellExecuteW（参数 = riv 路径，完全对齐 server_main.cpp
// LaunchPetIfInstalled 的拉起方式）。本进程直接拉起，不依赖 server 运行状态
// （server 被停/被杀时语言栏菜单仍能打开桌宠）。
void LaunchPetFromLangBar() {
  const std::wstring dir = RimeServerInstallDir();
  if (dir.empty()) {
    LOG("[tsf] 注册表无 Beautype InstallDir，跳过打开桌宠\n");
    return;
  }
  const std::wstring exe = dir + L"\\pet\\pet_preview.exe";
  const std::wstring riv = dir + L"\\pet\\" + kDefaultRivName;
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES ||
      GetFileAttributesW(riv.c_str()) == INVALID_FILE_ATTRIBUTES) {
    LOG("[tsf] 桌宠组件缺失（%ls / %ls），跳过打开桌宠\n", exe.c_str(),
        riv.c_str());
    return;
  }
  const std::wstring args = L"\"" + riv + L"\"";
  const HINSTANCE hr = ::ShellExecuteW(nullptr, L"open", exe.c_str(),
                                       args.c_str(), dir.c_str(), SW_SHOWNORMAL);
  LOG("[tsf] 打开桌宠 pet_preview.exe ret=%d（组件: %ls）\n",
      static_cast<int>(reinterpret_cast<intptr_t>(hr)), exe.c_str());
}

// Windows 虚拟键码（WM_KEYDOWN wParam）→ librime keysym。
// librime 输入约定与 X11/weasel 一致：字母用「小写 keysym」（0x61-0x7A），
// 大写经 shift 修饰位表达。直接把 VK 大写码（0x41-0x5A）喂给引擎会被当作
// 大写字母，luna_pinyin 拼写匹配失败 → 无中文候选、空格直接上屏原文
// （真机实测：NINIHAO / MEIYOU / A）。常量值取自 librime 自带
// include/X11/keysymdef.h（与 X11、weasel 同源）。
//
// 标点键的 shift 处理（真机实测根因）：X11/weasel 约定标点的 keysym 已表达
// 「shift 上档字符」——Shift+/ 的 keysym 是 XK_question(0x3F) 而非 XK_slash(0x2F)，
// librime punctuator 正是按这个 keysym 查表命中 ？。修复前固定返回无 shift 的
// keysym（0x2F），shift+/ 落入 half_shape 的 '/' → 输出 、 而非 ？。
// 故这里按 shift 状态返回「上档 keysym」。
// caps_on：CapsLock 开启。对齐官方 ConvertKeyEvent（WeaselTSF\KeyEvent.cpp:
// ToUnicodeEx 用键盘状态含 caps）——caps 开启时字母输出大写字符 XK_A-Z，
// librime 对 XK_A-Z 作 ascii 直出（大写字母不参与拼音组合）；caps 与 shift
// 异或（标准 caps 行为：caps 开 + shift = 小写）。
int VkToRimeKey(int vk, bool shift_down, bool caps_on, int scan_code,
                bool extended) {
  // 字母：VK_A-Z(0x41-0x5A) → XK_a-z(0x61-0x7A)，恒小写。caps 状态由发送的
  // mask LockMask 表达，librime 的 KeyEvent 归一化会自动把「小写 keycode +
  // LockMask」转成大写键（官方 ConvertKeyEvent 语义：keycode 恒小写、修饰
  // 位表达大小写）。修复前 caps 时发大写 XK_A-Z + LockMask → librime 再把
  // 大写+Lock 归一化回小写 → caps 开启却打出小写（真机 2026-10-07）。
  if (vk >= 'A' && vk <= 'Z') {
    (void)shift_down;  // shift/caps 均由发送的 mask 表达，keycode 恒小写
    (void)caps_on;
    return vk + ('a' - 'A');  // XK_a-z（大小写交给 librime + mask）
  }
  // 数字：永远返回基础 keycode（0x30-0x39），Shift 状态放在 mask 里由 librime
  // 归一化处理。对齐 weasel KeyEvent.cpp：X11 keysym 约定 keysym 恒为未上档字符，
  // 上档由 mask 表达。修复前 shift 时返回上档字符（1→! 2→@），导致 rime key_binder
  // 配置的 Control+Shift+1 收到的是 Control+! 而非 Control+Shift+1，全部匹配失败。
  if (vk >= '0' && vk <= '9') {
    return vk;
  }
  if (vk == VK_SPACE) {
    return 0x0020;  // XK_space
  }
  if (vk >= VK_F1 && vk <= VK_F12) {
    return 0xFFBE + (vk - VK_F1);  // XK_F1(0xFFBE) .. XK_F12(0xFFC9)
  }
  switch (vk) {
    case VK_RETURN: return 0xFF0D;   // XK_Return
    case VK_BACK: return 0xFF08;     // XK_BackSpace
    case VK_TAB: return 0xFF09;      // XK_Tab
    case VK_ESCAPE: return 0xFF1B;   // XK_Escape
    case VK_SELECT: return VK_SELECT;  // 0x29：点击候选后服务端 SendInput 的模拟键，
                                       // 服务端 select_pending_ 分支按 keycode==VK_SELECT
                                       // 消费并回传 commit（beautype_handler.cpp:318）
    case VK_CAPITAL: return 0xFFE5;  // XK_Caps_Lock（补键：对齐 weasel，caps
                                     // 键也要进引擎；切换仍由系统完成）
    case VK_SHIFT:                   // 对齐官方 TranslateKeycode
                                     // （KeyEvent.cpp:75-80）：scanCode 0x36
                                     // 是右 Shift → XK_Shift_R，否则 XK_Shift_L
      return (scan_code == 0x36) ? 0xFFE2 : 0xFFE1;
    case VK_CONTROL:                 // 官方 KeyEvent.cpp:81-86：isExtended →
                                     // XK_Control_R，否则 XK_Control_L
      return extended ? 0xFFE4 : 0xFFE3;
    case VK_MENU: return 0xFFE9;     // XK_Alt_L（官方 VK_MENU 恒 Alt_L）
    case VK_LWIN: return 0xFFE7;     // XK_Meta_L（官方 KeyEvent.cpp:143-144）
    case VK_RWIN: return 0xFFE8;     // XK_Meta_R
    case VK_PRIOR: return 0xFF55;    // XK_Page_Up
    case VK_NEXT: return 0xFF56;     // XK_Page_Down
    case VK_END: return 0xFF57;      // XK_End
    case VK_HOME: return 0xFF50;     // XK_Home
    case VK_LEFT: return 0xFF51;     // XK_Left
    case VK_UP: return 0xFF52;       // XK_Up
    case VK_RIGHT: return 0xFF53;    // XK_Right
    case VK_DOWN: return 0xFF54;     // XK_Down
    case VK_INSERT: return 0xFF63;   // XK_Insert
    case VK_DELETE: return 0xFFFF;   // XK_Delete
    // 标点：无 shift → 半角 keysym；shift → 上档 keysym（X11 keysymdef.h 值）
    case VK_OEM_1: return shift_down ? 0x003A : 0x003B;  // XK_colon : / XK_semicolon ;
    case VK_OEM_PLUS: return shift_down ? 0x002B : 0x003D;  // XK_plus + / XK_equal =
    case VK_OEM_COMMA: return shift_down ? 0x003C : 0x002C;  // XK_less < / XK_comma ,
    case VK_OEM_MINUS: return shift_down ? 0x005F : 0x002D;  // XK_underscore _ / XK_minus -
    case VK_OEM_PERIOD: return shift_down ? 0x003E : 0x002E;  // XK_greater > / XK_period .
    case VK_OEM_2: return shift_down ? 0x003F : 0x002F;  // XK_question ? / XK_slash /
    case VK_OEM_3: return shift_down ? 0x007E : 0x0060;  // XK_asciitilde ~ / XK_grave `
    case VK_OEM_4: return shift_down ? 0x007B : 0x005B;  // XK_braceleft { / XK_bracketleft [
    case VK_OEM_5: return shift_down ? 0x007C : 0x005C;  // XK_bar | / XK_backslash
    case VK_OEM_6: return shift_down ? 0x007D : 0x005D;  // XK_braceright } / XK_bracketright ]
    case VK_OEM_7: return shift_down ? 0x0022 : 0x0027;  // XK_quotedbl " / XK_apostrophe '
    default: return 0;  // 无 keysym 映射（未识别系统键）→ 调用方走官方
                        // ToUnicodeEx 兜底（KeyEvent.cpp:44-55）
  }
}

// 上屏编辑会话：TSF 的 edit cookie 仅在 RequestEditSession 回调（DoEditSession）
// 内有效，不能缓存激活时的 cookie 直接用（微软文档 TfEditCookie 语义）。
// 小狼毫 WeaselTSF 同样走 RequestEditSession + ITfEditSession 上屏。
class CommitEditSession final : public ITfEditSession {
 public:
  CommitEditSession(ITfContext* ctx, std::wstring text)
      : ctx_(ctx), text_(std::move(text)) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG n = --ref_;
    if (n == 0) delete this;
    return n;
  }
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    ITfInsertAtSelection* insert = nullptr;
    HRESULT hr = ctx_->QueryInterface(IID_ITfInsertAtSelection,
                                      reinterpret_cast<void**>(&insert));
    if (SUCCEEDED(hr) && insert != nullptr) {
      ITfRange* range = nullptr;
      hr = insert->InsertTextAtSelection(ec, 0, text_.c_str(),
                                         static_cast<LONG>(text_.size()), &range);
      if (range != nullptr) range->Release();
      insert->Release();
    }
    return S_OK;
  }

 private:
  ITfContext* ctx_;
  std::wstring text_;
  volatile ULONG ref_ = 1;
};

// 候选窗定位编辑会话：对齐小狼毫 CGetTextExtentEditSession —— 在编辑会话内用
// ITfContextView::GetTextExt 取「组合文本 / 光标选区」的屏幕矩形，候选窗显示
// 在光标下方。真机根因 2026-10-05：此前用 GetScreenExt（整个文档/窗口矩形），
// 候选窗被钉在窗口左上角。GetTextExt 失败（应用不提供精确布局）时降级
// GetScreenExt。有行内组合时优先用组合 range（weasel 同款），否则用光标选区。
class CandidatePositionEditSession final : public ITfEditSession {
 public:
  CandidatePositionEditSession(ime::RimeTextService* svc, ITfContext* ctx,
                               ITfComposition* comp,
                               ime::ui::CandidateWindow* win,
                               std::vector<std::string> texts, int highlighted)
      : svc_(svc), ctx_(ctx), comp_(comp), win_(win),
        texts_(std::move(texts)), highlighted_(highlighted) {
    if (svc_ != nullptr) svc_->AddRef();
    if (ctx_ != nullptr) ctx_->AddRef();
  }
  ~CandidatePositionEditSession() {
    if (svc_ != nullptr) svc_->Release();
    if (ctx_ != nullptr) ctx_->Release();
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG n = --ref_;
    if (n == 0) delete this;
    return n;
  }
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    // 异步会话可能在 Deactivate 后执行：服务未激活则不显示（防悬垂）
    if (svc_ == nullptr || !svc_->activated() || ctx_ == nullptr) {
      return S_OK;
    }
    RECT rc{};
    bool positioned = false;
    ITfContextView* view = nullptr;
    // 官方 weasel 取 view 用 ITfContext::GetActiveView（Composition.cpp:215 /
    // CandidateList.cpp:257 / LanguageBar.cpp:348 三处一致）。此前误用
    // QueryInterface(IID_ITfContextView) —— ITfContext 对象不实现该接口，QI 必
    // 失败 → GetTextExt 两路全跳过 → fallback GetCaretPos 对无 caret 应用
    // （浏览器/Electron 等）返回 (0,0) → 候选窗钉左上角。
    // 真机根因 2026-10-06：rime_tsf.log 连续 40+ 条 "fallback GetCaretPos=(0,0)"。
    if (SUCCEEDED(ctx_->GetActiveView(&view)) && view != nullptr) {
      // 1) 优先：行内组合的 range（weasel 同款，位置最准确）
      if (comp_ != nullptr) {
        ITfRange* comp_range = nullptr;
        if (SUCCEEDED(comp_->GetRange(&comp_range)) && comp_range != nullptr) {
          // 锚组合 END（= 光标，官方 CInlinePreeditEditSession 把光标设在组合
          // 末尾）：候选窗跟随输入点。用 START 会锚到拼音串起点，与 sel
          // fallback（光标）差一个 preedit 宽度，comp/sel 交替时候选窗左右闪
          // （真机根因 2026-10-13：上报 rc 在 830↔956 交替闪动）。
          comp_range->Collapse(ec, TF_ANCHOR_END);
          BOOL clipped = FALSE;
          // weasel CGetTextExtentEditSession: 只检查 S_OK && (rc.left!=0||rc.top!=0)，
          // 不检查 clipped（很多应用 clipped=TRUE 但坐标仍正确）
          const HRESULT hrExt = view->GetTextExt(ec, comp_range, &rc, &clipped);
          if (SUCCEEDED(hrExt) && (rc.left != 0 || rc.top != 0)) {
            positioned = true;
          } else if (::ime::g_log) {
            ::ime::LogF(
                         "[candidate] comp GetTextExt hr=0x%08X rc=(%d,%d)\n",
                         hrExt, rc.left, rc.top);
            std::fflush(::ime::g_log);
          }
          comp_range->Release();
        }
      }
      if (!positioned) {
        TF_SELECTION sel = {};
        ULONG count = 0;
        if (SUCCEEDED(ctx_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel,
                                         &count)) &&
            count > 0 && sel.range != nullptr) {
          BOOL clipped = FALSE;
          const HRESULT hrExt = view->GetTextExt(ec, sel.range, &rc, &clipped);
          if (SUCCEEDED(hrExt) && (rc.left != 0 || rc.top != 0)) {
            positioned = true;
          } else if (::ime::g_log) {
            ::ime::LogF(
                         "[candidate] sel GetTextExt hr=0x%08X rc=(%d,%d)\n",
                         hrExt, rc.left, rc.top);
            std::fflush(::ime::g_log);
          }
          sel.range->Release();
        }
      }
      view->Release();
    } else if (::ime::g_log) {
      ::ime::LogF( "[candidate] GetActiveView FAILED\n");
      std::fflush(::ime::g_log);
    }
    // weasel enhanced_position: GetTextExt 成功但坐标在前台窗口外时，
    // 用 GetCaretPos 偏移修正（Word/UWP 等应用坐标系统一问题）
    if (positioned) {
      HWND fg = GetForegroundWindow();
      RECT rcFg{};
      if (fg && GetWindowRect(fg, &rcFg)) {
        if (rc.left < rcFg.left || rc.left > rcFg.right ||
            rc.top < rcFg.top || rc.top > rcFg.bottom) {
          POINT pt{};
          BOOL hasCaret = GetCaretPos(&pt);
          int ox = rcFg.left - rc.left + (hasCaret ? pt.x : 0);
          int oy = rcFg.top - rc.top + (hasCaret ? pt.y : 0);
          rc.left += ox; rc.right += ox;
          rc.top += oy; rc.bottom += oy;
        }
      }
    }
    if (!positioned) {
      POINT pt{};
      bool ok = GetCaretPos(&pt) && (pt.x != 0 || pt.y != 0);
      if (!ok) {
        // 无 caret 应用（浏览器/Electron/部分 UWP）：取前台线程的 caret 矩形
        // （GetGUIThreadInfo 比 GetCaretPos 更可靠，带 hwndCaret 可换算屏幕坐标）
        HWND fg = GetForegroundWindow();
        DWORD tid = (fg != nullptr) ? GetWindowThreadProcessId(fg, nullptr) : 0;
        GUITHREADINFO gti{};
        gti.cbSize = sizeof(gti);
        if (tid != 0 && GetGUIThreadInfo(tid, &gti) && gti.hwndCaret != nullptr) {
          POINT cpt{gti.rcCaret.left, gti.rcCaret.bottom};
          ClientToScreen(gti.hwndCaret, &cpt);
          pt.x = cpt.x;
          pt.y = cpt.y;
          ok = true;
        } else if (tid != 0 && GetGUIThreadInfo(tid, &gti) &&
                   gti.hwndFocus != nullptr) {
          // 最终兜底：输入焦点窗口底部中央（绝不钉屏幕左上角 (0,0)）
          RECT rw{};
          if (GetWindowRect(gti.hwndFocus, &rw)) {
            pt.x = (rw.left + rw.right) / 2;
            pt.y = rw.bottom;
            ok = true;
          }
        }
      }
      if (ok) {
        // 屏幕范围校验：fallback 的 GetCaretPos/GetGUIThreadInfo 可能给出负坐标
        // 或屏幕外坐标（真机根因 2026-10-06：fallback caret=(-1280,1466) 候选窗
        // 显示到屏幕外），越界则改用前台焦点窗口底部中央，绝不钉屏幕外。
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        if (pt.x < wa.left || pt.x > wa.right || pt.y < wa.top ||
            pt.y > wa.bottom) {
          HWND fg2 = GetForegroundWindow();
          RECT rw{};
          if (fg2 != nullptr && GetWindowRect(fg2, &rw)) {
            pt.x = (rw.left + rw.right) / 2;
            pt.y = rw.bottom;
          } else {
            pt.x = wa.left + (wa.right - wa.left) / 2;
            pt.y = wa.top + (wa.bottom - wa.top) / 2;
          }
        }
        rc.left = pt.x; rc.top = pt.y;
        rc.right = pt.x + 20; rc.bottom = pt.y + 16;
        positioned = true;
        if (::ime::g_log) {
          ::ime::LogF(
                       "[candidate] GetTextExt 失败 fallback caret=(%d,%d)\n",
                       pt.x, pt.y);
          std::fflush(::ime::g_log);
        }
      }
    }
    if (positioned) {
      win_->SetPosition(rc.left, rc.bottom + 4);
      if (::ime::g_log) {
        ::ime::LogF(
                     "[candidate] 定位 rc=(%d,%d)-(%d,%d) -> win@(%d,%d)\n",
                     rc.left, rc.top, rc.right, rc.bottom,
                     rc.left, rc.bottom + 4);
        std::fflush(::ime::g_log);
      }
    } else {
      if (::ime::g_log) {
        ::ime::LogF( "[candidate] 定位全失败\n");
        std::fflush(::ime::g_log);
      }
    }
    win_->Show(texts_, highlighted_);
    return S_OK;
  }

 private:
  ime::RimeTextService* svc_;  // 保活（AddRef），异步会话期间服务不删除
  ITfContext* ctx_;            // 保活（AddRef）
  ITfComposition* comp_;       // 非拥有（服务持有）
  ime::ui::CandidateWindow* win_;
  std::vector<std::string> texts_;
  int highlighted_;
  volatile ULONG ref_ = 1;
};

// 服务端候选窗定位会话（照官方 CGetTextExtentEditSession，Composition.cpp:
// 159-245）：EditSession 内 GetActiveView + 组合 range/GetSelection + GetTextExt
// 拿组合区屏幕坐标，带 enhanced_position 修正与兜底链，最后通过
// svc_->ReportServerPosition 上报给服务端候选窗。
// 此前 UpdateCandidateWindow 用 GetCaretPos+前台窗口换算：无 caret 应用
// （浏览器/Electron 等）返回 (0,0) → 服务端候选窗钉左上角（真机根因）。
class ServerPositionEditSession final : public ITfEditSession {
 public:
  ServerPositionEditSession(ime::RimeTextService* svc, ITfContext* ctx,
                            ITfComposition* comp)
      : svc_(svc), ctx_(ctx), comp_(comp) {
    if (svc_ != nullptr) svc_->AddRef();
    if (ctx_ != nullptr) ctx_->AddRef();
  }
  ~ServerPositionEditSession() {
    if (svc_ != nullptr) svc_->Release();
    if (ctx_ != nullptr) ctx_->Release();
  }

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG n = --ref_;
    if (n == 0) delete this;
    return n;
  }
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    // 异步会话可能在 Deactivate 后执行：服务未激活则不处理（防悬垂）
    if (svc_ == nullptr || !svc_->activated() || ctx_ == nullptr) {
      return S_OK;
    }
    RECT rc{};
    bool positioned = false;
    ITfContextView* view = nullptr;
    // 官方 weasel 取 view 用 ITfContext::GetActiveView（Composition.cpp:215）
    if (SUCCEEDED(ctx_->GetActiveView(&view)) && view != nullptr) {
      // 官方 CGetTextExtentEditSession（Composition.cpp:159-210）正流程：
      // 一次选 range、一次 GetTextExt——组合存在时用组合 range（锚 START，
      // 拼音起点），组合不存在（组合结束）时用 selection（光标）。
      ITfRange* pRange = nullptr;
      if (comp_ != nullptr) {
        // 官方同款：组合存在 → 只用组合 range，锚 TF_ANCHOR_START（拼音起点）。
        // 成熟验证行为：候选窗锚组合起点、不随拼音增长横移（微软系跟随光标
        // 是另一种习惯，官方用户已用多年验证稳定）。
        // 此前 Beautype 自研：锚 END（光标）+ comp GetTextExt 失败再 fallback
        // sel——两来源锚点差一个 preedit 宽度 → comp 成功(END 光标) 与 sel
        // fallback(光标) 交替实际不闪，但 comp 成功(START 旧版) 与 sel 交替
        // 830↔956 左右闪（真机 2026-10-13）。对齐官方后 comp 失败直接保持
        // 原位（不报），不再有双来源交替，锚点可安全回 START。
        ITfRange* comp_range = nullptr;
        if (SUCCEEDED(comp_->GetRange(&comp_range)) && comp_range != nullptr) {
          comp_range->Collapse(ec, TF_ANCHOR_START);
          pRange = comp_range;
        }
      } else {
        // 组合不存在（官方 Composition.cpp:178-180 "composition end" 分支）：
        // 官方直接用 selection.range（空 range = 光标位置），保持服务端位置
        // 与光标同步。
        TF_SELECTION sel = {};
        ULONG count = 0;
        if (SUCCEEDED(ctx_->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &sel,
                                         &count)) &&
            count > 0 && sel.range != nullptr) {
          pRange = sel.range;
        }
      }
      if (pRange != nullptr) {
        BOOL clipped = FALSE;
        // weasel: 只检查 S_OK && (rc.left!=0||rc.top!=0)，不检查 clipped
        const HRESULT hrExt = view->GetTextExt(ec, pRange, &rc, &clipped);
        if (SUCCEEDED(hrExt) && (rc.left != 0 || rc.top != 0)) {
          positioned = true;
        } else if (::ime::g_log) {
          ::ime::LogF("[candidate] server GetTextExt hr=0x%08X rc=(%d,%d)\n",
                      hrExt, rc.left, rc.top);
          std::fflush(::ime::g_log);
        }
        pRange->Release();
      } else if (::ime::g_log) {
        ::ime::LogF("[candidate] server 无组合且无 selection，保持原位不跳\n");
        std::fflush(::ime::g_log);
      }
      view->Release();
    }
    // 3) enhanced_position 修正：GetTextExt 坐标在前台窗口外时按 caret 偏移
    //    （weasel CGetTextExtentEditSession 同款，Word/UWP 坐标系统一）
    if (positioned) {
      HWND fg = GetForegroundWindow();
      RECT rcFg{};
      if (fg != nullptr && GetWindowRect(fg, &rcFg)) {
        if (rc.left < rcFg.left || rc.left > rcFg.right ||
            rc.top < rcFg.top || rc.top > rcFg.bottom) {
          POINT pt{};
          const BOOL hasCaret = GetCaretPos(&pt);
          const int ox = rcFg.left - rc.left + (hasCaret ? pt.x : 0);
          const int oy = rcFg.top - rc.top + (hasCaret ? pt.y : 0);
          rc.left += ox; rc.right += ox;
          rc.top += oy; rc.bottom += oy;
        }
      }
    }
    // 4) GetTextExt 两路全失败：对齐官方 weasel CGetTextExtentEditSession
    //    （Composition.cpp:213-245）——失败时返回 S_FALSE、不更新输入位置，
    //    候选窗保持原位。禁止自研兜底假位置（真机 2026-10-13 实证：豆包
    //    OnLayoutChange 时刻 GetTextExt 失败 → 兜底链产出恒定假坐标
    //    (3840,1462)-(3860,1478)（20×16 兜底构造尺寸）与真值交替上报 →
    //    候选窗在真/假位置间反复横跳 = 用户所见「候选框到处闪动」；
    //    微软输入法/小狼毫失败时都是保持原位不跳）。
    if (!positioned) {
      ::ime::LogF("[candidate] server 定位 GetTextExt 两路全失败，保持原位不跳\n");
      std::fflush(::ime::g_log);
    }
    if (positioned) {
      svc_->ReportServerPosition(rc);
    }
    return S_OK;
  }

 private:
  ime::RimeTextService* svc_;  // 保活（AddRef），异步会话期间服务不删除
  ITfContext* ctx_;            // 保活（AddRef）
  ITfComposition* comp_;       // 非拥有（服务持有）
  volatile ULONG ref_ = 1;
};

// 行内组合编辑会话族（对齐小狼毫 Composition.cpp 的 CStartComposition /
// CEndComposition / CInlinePreedit / CInsertText 系列）。
// 开始组合：QUERYONLY 插入点 range → StartComposition → 光标到组合末尾。
class StartCompositionEditSession final : public ITfEditSession {
 public:
  StartCompositionEditSession(ime::RimeTextService* svc, ITfContext* ctx)
      : svc_(svc), ctx_(ctx) {}
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG n = --ref_;
    if (n == 0) delete this;
    return n;
  }
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    ITfInsertAtSelection* insert = nullptr;
    if (FAILED(ctx_->QueryInterface(IID_ITfInsertAtSelection,
                                    reinterpret_cast<void**>(&insert)))) {
      return E_FAIL;
    }
    ITfRange* range = nullptr;
    HRESULT hr = insert->InsertTextAtSelection(ec, TF_IAS_QUERYONLY, nullptr,
                                               0, &range);
    insert->Release();
    if (FAILED(hr) || range == nullptr) {
      return E_FAIL;
    }
    ITfContextComposition* ctx_comp = nullptr;
    ITfComposition* composition = nullptr;
    hr = ctx_->QueryInterface(IID_ITfContextComposition,
                              reinterpret_cast<void**>(&ctx_comp));
    if (SUCCEEDED(hr) && ctx_comp != nullptr) {
      hr = ctx_comp->StartComposition(ec, range, svc_, &composition);
      ctx_comp->Release();
    }
    if (SUCCEEDED(hr) && composition != nullptr) {
      svc_->SetComposition(composition);
      // 光标放到组合末尾
      range->Collapse(ec, TF_ANCHOR_END);
      TF_SELECTION tf_sel = {};
      tf_sel.range = range;
      tf_sel.style.ase = TF_AE_NONE;
      tf_sel.style.fInterimChar = FALSE;
      ctx_->SetSelection(ec, 1, &tf_sel);
      // 2026-10-14 根因修复：此处不再对空 range 设置显示属性（官方 weasel
      // CStartCompositionEditSession 从不设置属性）。log 实证：组合刚创建时
      // range 为空（插入点），空 range SetValue 必失败 E_INVALIDARG(0x80070057)，
      // 且 TSF 会在插入点留下"脏属性段"——Chromium/豆包枚举到该脏段画出
      // "只有光标旁一点点"的下划线而非全文组合段（微软 IME 无此脏段故全文
      // 正常）。显示属性改由 InlinePreeditEditSession 在 SetText 全文后设置。
      // weasel 同款：StartComposition 成功后立即发定位会话
      // （CStartCompositionEditSession::DoEditSession 末尾调 _UpdateCompositionWindow）
      svc_->OnCompositionStarted(ctx_);
      composition->Release();
    }
    range->Release();
    return S_OK;
  }

 private:
  ime::RimeTextService* svc_;
  ITfContext* ctx_;
  volatile ULONG ref_ = 1;
};

// 结束组合：clear=true 清空组合文本（取消/失焦）；false 保留已提交文本。
class EndCompositionEditSession final : public ITfEditSession {
 public:
  EndCompositionEditSession(ime::RimeTextService* svc, ITfContext* ctx,
                            ITfComposition* comp, bool clear)
      : svc_(svc), ctx_(ctx), comp_(comp), clear_(clear) {}
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG n = --ref_;
    if (n == 0) delete this;
    return n;
  }
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    if (comp_ == nullptr) {
      return S_OK;
    }
    ITfRange* cr = nullptr;
    if (SUCCEEDED(comp_->GetRange(&cr)) && cr != nullptr) {
      svc_->ClearCompositionDisplayAttributes(ec, ctx_, cr);
      cr->Release();
    }
    HRESULT hrClearText = S_OK;
    if (clear_) {
      ITfRange* range = nullptr;
      if (SUCCEEDED(comp_->GetRange(&range)) && range != nullptr) {
        hrClearText = range->SetText(ec, 0, L"", 0);
        range->Release();
      }
    }
    // 先让服务释放组合引用再 EndComposition（weasel：避免同步回调误判外部中止）
    svc_->FinalizeComposition();
    const HRESULT hrEnd = comp_->EndComposition(ec);
    ::ime::LogF(
        "[candidate] 提交 EndComposition=0x%08lX 清空文本=0x%08lX\n",
        static_cast<unsigned long>(hrEnd),
        static_cast<unsigned long>(hrClearText));
    std::fflush(::ime::g_log);
    return S_OK;
  }

 private:
  ime::RimeTextService* svc_;
  ITfContext* ctx_;
  ITfComposition* comp_;
  bool clear_;
  volatile ULONG ref_ = 1;
};

// 更新行内预编辑文本 + 光标（weasel CInlinePreeditEditSession）。
class InlinePreeditEditSession final : public ITfEditSession {
 public:
  InlinePreeditEditSession(ime::RimeTextService* svc, ITfContext* ctx,
                           ITfComposition* comp, std::wstring preedit,
                           int sel_cursor)
      : svc_(svc), ctx_(ctx), comp_(comp), preedit_(std::move(preedit)),
        sel_cursor_(sel_cursor) {}
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG n = --ref_;
    if (n == 0) delete this;
    return n;
  }
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    if (comp_ == nullptr) {
      return E_FAIL;
    }
    ITfRange* range = nullptr;
    if (FAILED(comp_->GetRange(&range)) || range == nullptr) {
      return E_FAIL;
    }
    HRESULT hrText = range->SetText(ec, 0, preedit_.c_str(),
                                    static_cast<LONG>(preedit_.length()));
    if (FAILED(hrText)) {
      range->Release();
      return E_FAIL;
    }
    BOOL empty = FALSE;
    range->IsEmpty(ec, &empty);
    svc_->SetCompositionDisplayAttributes(ec, ctx_, range);
    // 光标：sel_cursor<0 → 组合末尾；否则组合开头 + ShiftStart(sel_cursor)
    if (sel_cursor_ < 0) {
      range->Collapse(ec, TF_ANCHOR_END);
    } else {
      range->Collapse(ec, TF_ANCHOR_START);
      LONG cch = 0;
      range->ShiftStart(ec, sel_cursor_, &cch, nullptr);
    }
    TF_SELECTION tf_sel = {};
    tf_sel.range = range;
    tf_sel.style.ase = TF_AE_NONE;
    tf_sel.style.fInterimChar = FALSE;
    HRESULT hrSel = ctx_->SetSelection(ec, 1, &tf_sel);
    // 2026-10-14 下划线诊断：组合文本写入/属性/光标设置全链路 HRESULT
    ::ime::LogF(
        "[tsf] 行内更新 preedit=%ls settext=0x%08lX range空=%d setsel=0x%08lX\n",
        preedit_.c_str(), static_cast<unsigned long>(hrText), empty ? 1 : 0,
        static_cast<unsigned long>(hrSel));
    std::fflush(::ime::g_log);
    range->Release();
    return S_OK;
  }

 private:
  ime::RimeTextService* svc_;
  ITfContext* ctx_;
  ITfComposition* comp_;
  std::wstring preedit_;
  int sel_cursor_;
  volatile ULONG ref_ = 1;
};

// 上屏提交：组合存在时把 commit 写入组合 range（替换拼音）并结束组合
//（weasel CInsertTextEditSession + EndComposition(clear=false) 提交路径）。
class CommitCompositionEditSession final : public ITfEditSession {
 public:
  CommitCompositionEditSession(ime::RimeTextService* svc, ITfContext* ctx,
                               ITfComposition* comp, std::wstring commit)
      : svc_(svc), ctx_(ctx), comp_(comp), commit_(std::move(commit)) {}
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfEditSession)) {
      *ppv = static_cast<ITfEditSession*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG n = --ref_;
    if (n == 0) delete this;
    return n;
  }
  STDMETHODIMP DoEditSession(TfEditCookie ec) override {
    if (comp_ == nullptr) {
      return E_FAIL;
    }
    ITfRange* range = nullptr;
    if (FAILED(comp_->GetRange(&range)) || range == nullptr) {
      ::ime::LogF( "[candidate] 提交 FAIL: GetRange 失败\n");
      return E_FAIL;
    }
    // 用 commit 文本替换组合里的拼音
    const HRESULT hrSet = range->SetText(ec, 0, commit_.c_str(),
                                         static_cast<LONG>(commit_.length()));
    ::ime::LogF( "[candidate] 提交 SetText=0x%08lX len=%d\n",
                 static_cast<unsigned long>(hrSet),
                 static_cast<int>(commit_.length()));
    if (FAILED(hrSet)) {
      range->Release();
      return E_FAIL;
    }
    // 光标移到提交文本末尾
    range->Collapse(ec, TF_ANCHOR_END);
    TF_SELECTION tf_sel = {};
    tf_sel.range = range;
    tf_sel.style.ase = TF_AE_NONE;
    tf_sel.style.fInterimChar = FALSE;
    ctx_->SetSelection(ec, 1, &tf_sel);
    range->Release();
    // 结束组合（不清空文本——commit 已在 range 内，保留提交）
    ITfRange* cr = nullptr;
    if (SUCCEEDED(comp_->GetRange(&cr)) && cr != nullptr) {
      svc_->ClearCompositionDisplayAttributes(ec, ctx_, cr);
      cr->Release();
    }
    svc_->FinalizeComposition();
    const HRESULT hrEnd = comp_->EndComposition(ec);
    ::ime::LogF( "[candidate] 提交 EndComposition=0x%08lX\n",
                 static_cast<unsigned long>(hrEnd));
    return S_OK;
  }

 private:
  ime::RimeTextService* svc_;
  ITfContext* ctx_;
  ITfComposition* comp_;
  std::wstring commit_;
  volatile ULONG ref_ = 1;
};

// 行内组合显示属性信息（TF_DISPLAYATTRIBUTE：下划线样式，对齐微软 TSF Sample
// 与 weasel 的组合下划线）。
class DisplayAttributeInfoImpl final : public ITfDisplayAttributeInfo {
 public:
  explicit DisplayAttributeInfoImpl(const GUID& guid) : guid_(guid) {}
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_ITfDisplayAttributeInfo)) {
      *ppv = static_cast<ITfDisplayAttributeInfo*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG n = --ref_;
    if (n == 0) delete this;
    return n;
  }
  STDMETHODIMP GetGUID(GUID* pguid) override {
    if (pguid == nullptr) return E_INVALIDARG;
    *pguid = guid_;
    return S_OK;
  }
  STDMETHODIMP GetDescription(BSTR* pbstrDesc) override {
    if (pbstrDesc == nullptr) return E_INVALIDARG;
    ime::i18n::ReloadLanguage();
    *pbstrDesc = SysAllocString(
        ime::i18n::TrW(ime::i18n::Str::kCompositionDescription));
    return (*pbstrDesc == nullptr) ? E_OUTOFMEMORY : S_OK;
  }
  STDMETHODIMP GetAttributeInfo(TF_DISPLAYATTRIBUTE* pAttr) override {
    if (pAttr == nullptr) return E_INVALIDARG;
    // 组合文本：白底黑字 + 灰色实线下划线（用户要求 2026-10-14：原蓝色
    // RGB(0,0,255) 改为中性灰 RGB(128,128,128)）
    pAttr->crText.type = TF_CT_COLORREF;
    pAttr->crText.cr = RGB(0, 0, 0);
    pAttr->crBk.type = TF_CT_COLORREF;
    pAttr->crBk.cr = RGB(255, 255, 255);
    pAttr->lsStyle = TF_LS_SOLID;
    pAttr->fBoldLine = FALSE;
    pAttr->crLine.type = TF_CT_COLORREF;
    pAttr->crLine.cr = RGB(128, 128, 128);
    pAttr->bAttr = TF_ATTR_INPUT;
    return S_OK;
  }
  STDMETHODIMP SetAttributeInfo(const TF_DISPLAYATTRIBUTE*) override {
    return E_NOTIMPL;  // 只读属性
  }
  STDMETHODIMP Reset() override { return S_OK; }

 private:
  GUID guid_;
  volatile ULONG ref_ = 1;
};

}  // namespace

namespace ime {

RimeTextService::RimeTextService() = default;

void RimeTextService::SetTheme(const skin::Theme& theme) { candidate_window_.SetTheme(theme); }

void RimeTextService::SetConfigDir(const std::string& config_dir) {
  candidate_window_.SetConfigDir(config_dir);
}
void RimeTextService::SetCommitCallback(std::function<void(const std::string&)> cb) {
  commit_callback_ = std::move(cb);
}

void RimeTextService::HandleKeyUp(int vk, int mask) {
  // 标准 OnKeyUp 语义：IME 收到按键释放但不消费（librime 引擎不依赖 KEYUP）。
  LOG("[tsf] 收到按键释放 0x%02X（VK，mask=%d）——放行\n",
      static_cast<unsigned int>(vk), mask);
}

void RimeTextService::SetupCandidateInteractions() {
  candidate_window_.SetClickCallback([this](int index) { OnCandidateClicked(index); });
  candidate_window_.SetScrollCallback([this](bool up) { OnCandidateScroll(up); });
}

// demo 顶层窗口句柄（main.cpp 全局）：点候选时若 demo 被下层窗口盖住（穿透
// 事故），先把 demo 拉回前台再上屏，避免 commit 写到看不见的 EDIT 里。

void RimeTextService::OnCandidateClicked(int index) {
  // 服务化：鼠标点击候选 → 服务端 SelectCandidateOnCurrentPage（官方机制，
  // 服务端经 VK_SELECT 模拟键回传响应 → 本函数下一条 ProcessKey 前先消费）。
  if (!activated_ || index < 0 || !EnsureSession()) {
    return;
  }
  LOG("[tsf] 候选鼠标点选 #%d\n", index);
  client_.SelectCandidateOnCurrentPage(static_cast<std::size_t>(index));
  client_.GetResponseData(std::ref(parser_));
  last_commit_ = ToUtf8(last_commit_wide_);
  if (!last_commit_.empty()) {
    InsertCommitText(last_commit_);
    last_commit_.clear();
  }
  UpdateCandidateWindow();
}

void RimeTextService::OnCandidateScroll(bool up) {
  // 服务化：滚轮翻页 → 服务端 ChangePage（官方机制）
  if (!activated_ || !EnsureSession()) {
    return;
  }
  LOG("[tsf] 候选滚轮翻页（%s）\n", up ? "上" : "下");
  client_.ChangePage(up);
  client_.GetResponseData(std::ref(parser_));
  UpdateCandidateWindow();
}

STDMETHODIMP RimeTextService::QueryInterface(REFIID riid, void** ppv) {
  if (ppv == nullptr) {
    return E_POINTER;
  }
  *ppv = nullptr;
  // 对齐小狼毫 WeaselTSF：一个服务类暴露全部 TSF 接口族
  if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_ITfTextInputProcessor) ||
      IsEqualIID(riid, IID_ITfTextInputProcessorEx)) {
    // Ex 继承自 ITfTextInputProcessor：一个指针两个 IID 都成立
    *ppv = static_cast<ITfTextInputProcessorEx*>(this);
  } else if (IsEqualIID(riid, IID_ITfSource)) {
    *ppv = static_cast<ITfSource*>(this);
  } else if (IsEqualIID(riid, IID_ITfKeyEventSink)) {
    *ppv = static_cast<ITfKeyEventSink*>(this);
  } else if (IsEqualIID(riid, IID_ITfThreadMgrEventSink)) {
    *ppv = static_cast<ITfThreadMgrEventSink*>(this);
  } else if (IsEqualIID(riid, IID_ITfTextEditSink)) {
    *ppv = static_cast<ITfTextEditSink*>(this);
  } else if (IsEqualIID(riid, IID_ITfTextLayoutSink)) {
    *ppv = static_cast<ITfTextLayoutSink*>(this);
  } else if (IsEqualIID(riid, IID_ITfThreadFocusSink)) {
    *ppv = static_cast<ITfThreadFocusSink*>(this);
  } else if (IsEqualIID(riid, IID_ITfCompositionSink)) {
    *ppv = static_cast<ITfCompositionSink*>(this);
  } else if (IsEqualIID(riid, IID_ITfDisplayAttributeProvider)) {
    *ppv = static_cast<ITfDisplayAttributeProvider*>(this);
  } else if (IsEqualIID(riid, IID_ITfCompartmentEventSink)) {
    *ppv = static_cast<ITfCompartmentEventSink*>(this);
  } else if (IsEqualIID(riid, IID_ITfActiveLanguageProfileNotifySink)) {
    *ppv = static_cast<ITfActiveLanguageProfileNotifySink*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

STDMETHODIMP_(ULONG) RimeTextService::AddRef() { return ++ref_count_; }

STDMETHODIMP_(ULONG) RimeTextService::Release() {
  const ULONG n = --ref_count_;
  if (n == 0) {
    delete this;
  }
  return n;
}

STDMETHODIMP RimeTextService::Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId) {
  // v4.7.6：激活留痕（观测闭环——DLL 是否被系统加载/激活、由哪个进程）
  LOG("[tsf] Activate tfClientId=%u\n", static_cast<unsigned int>(tfClientId));
  // 对齐小狼毫：Activate 转 ActivateEx，全套初始化在 ActivateEx 完成
  return ActivateEx(pThreadMgr, tfClientId, 0U);
}

STDMETHODIMP RimeTextService::ActivateEx(ITfThreadMgr* pThreadMgr,
                                         TfClientId tfClientId,
                                         DWORD dwFlags) {
  if (pThreadMgr == nullptr) {
    return E_INVALIDARG;
  }
  thread_mgr_ = pThreadMgr;
  client_id_ = tfClientId;
  activate_flags_ = dwFlags;
  // 对齐小狼毫 WeaselTSF：不创建/不 push 自建文档上下文！TSF 键盘回调
  //（OnTestKeyDown/OnKeyDown）传入的 ITfContext* 就是焦点应用的上下文，
  // 组合/提交/定位全部作用在它上面。此前自建空上下文 push 导致：
  // ①提交写入空文档、应用看不到（“空格不上屏”）②空文档无布局、
  // GetTextExt 失败降级 GetScreenExt 钉左上角 ③编辑会话不稳定
  //（“输入框按不出来”）。真机根因 2026-10-06。
  document_mgr_ = nullptr;
  context_ = nullptr;
  HRESULT hr = S_OK;
  // ---- 对齐小狼毫 ActivateEx 初始化顺序（WeaselTSF.cpp）----
  // 1) 线程管理器事件 sink（文档管理器生命周期）
  InitThreadMgrEventSink();
  // 2) 焦点文档管理器的 TextEdit / TextLayout sink（外部修改文档/布局变化）
  ITfDocumentMgr* pDocMgrFocus = nullptr;
  if (SUCCEEDED(thread_mgr_->GetFocus(&pDocMgrFocus)) && pDocMgrFocus != nullptr) {
    InitTextEditSink(pDocMgrFocus);
    InitTextLayoutSink(pDocMgrFocus);
    pDocMgrFocus->Release();
  }
  // 3) 键盘事件 sink：TSF 文本服务必须经 ITfKeystrokeMgr::AdviseKeyEventSink
  //    注册 ITfKeyEventSink，否则按键不会送达（微软官方 Text Service Basics）。
  ITfKeystrokeMgr* keystroke_mgr = nullptr;
  hr = pThreadMgr->QueryInterface(IID_ITfKeystrokeMgr,
                                  reinterpret_cast<void**>(&keystroke_mgr));
  if (SUCCEEDED(hr) && keystroke_mgr != nullptr) {
    const HRESULT hrSink =
        keystroke_mgr->AdviseKeyEventSink(tfClientId, this, TRUE);
    keystroke_mgr->Release();
    LOG("[tsf] AdviseKeyEventSink(fForeground=TRUE): 0x%08lX\n",
        static_cast<unsigned long>(hrSink));
  } else {
    LOG("[tsf] FAIL: 获取 ITfKeystrokeMgr 失败 0x%08lX\n",
        static_cast<unsigned long>(hr));
  }
  // 4) 显示属性提供者（行内组合下划线）
  InitDisplayAttribute();
  // 5) 语言栏按钮（weasel：_InitLanguageBar 在 _SetKeyboardOpen 之前）
  InitLanguageBar();
  // 5.1) conversion compartment 监听（weasel Compartment.cpp）：系统中/英
  // 指示器、点击切换等能同步回引擎，避免"切到英文后点不回来"
  InitCompartment();
  // 6) UWP 集成候选（对齐 weasel _cand；每次候选刷新同步数据）
  candidate_ui_ = new (std::nothrow) ime::tsf::CandidateListUI(thread_mgr_);
  if (candidate_ui_ != nullptr) {
    candidate_ui_->SetAbortCallback([this]() { AbortComposition(); });
  }
  // 7) 键盘开关 compartment：激活时强制置开（weasel _SetKeyboardOpen(TRUE)）
  SetKeyboardOpen(TRUE);
  // 7.1) 激活语言档案通知 sink（对齐 weasel ActivateEx 中 ITfActiveLanguageProfile
  // NotifySink：重激活时语言栏状态刷新，避免"切走再切回指示消失"）
  InitActiveLanguageProfileNotifySink();
  // 7.2) 激活即恢复语言栏中/英 compartment 显示（对齐 weasel OnActivated →
  // _UpdateLanguageBar）
  UpdateLanguageBarCompartment();
  // 8) 线程焦点 sink（OnKillThreadFocus → 中止组合）
  InitThreadFocusSink();
  activated_ = true;
  SetupCandidateInteractions();
  // 9) 激活即连服务（对齐 weasel ActivateEx 末尾 _EnsureServerConnected()，
  // WeaselTSF.cpp:164）：服务进程不在时 EnsureSession 内含重试+自动拉起。
  // 连接失败不阻断激活（下次按键再重试，与官方一致）
  EnsureSession();
  LOG("[tsf] 服务激活（ActivateEx，flags=0x%lX）：文档管理器 + 上下文已入栈，"
      "sink 全家桶就绪（client_id=%llu）\n",
      static_cast<unsigned long>(dwFlags),
      static_cast<unsigned long long>(client_id_));
  return S_OK;
}

STDMETHODIMP RimeTextService::Deactivate() {
  // 对齐小狼毫 Deactivate：候选窗销毁 → 组合中止 → 逆序注销全部 sink
  candidate_window_.Hide();
  AbortComposition();
  // 服务化：结束 IPC 会话并断开（官方 Deactivate 同序）
  if (client_connected_) {
    if (session_started_) {
      client_.EndSession();
      session_started_ = false;
    }
    client_.Disconnect();
    client_connected_ = false;
  }
  SetKeyboardOpen(FALSE);
  // P2：语言栏按钮注销 + UWP 候选结束（weasel Deactivate 同序）
  UninitLanguageBar();
  UninitCompartment();
  UninitActiveLanguageProfileNotifySink();
  if (candidate_ui_ != nullptr) {
    candidate_ui_->DestroyAll();
    delete candidate_ui_;
    candidate_ui_ = nullptr;
  }
  UninitThreadFocusSink();
  UninitTextLayoutSink();
  UninitTextEditSink();
  UninitThreadMgrEventSink();
  UninitDisplayAttribute();
  // 先注销键盘事件 sink，再释放线程管理器引用
  if (thread_mgr_ != nullptr) {
    ITfKeystrokeMgr* keystroke_mgr = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(
            IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&keystroke_mgr))) &&
        keystroke_mgr != nullptr) {
      keystroke_mgr->UnadviseKeyEventSink(client_id_);
      keystroke_mgr->Release();
      LOG("[tsf] UnadviseKeyEventSink 完成\n");
    }
  }
  // 不再有自建文档/上下文（对齐 weasel：编辑目标=焦点应用上下文，由
  // 按键回调 pContext 提供，Deactivate 无需 Pop/Release）
  context_ = nullptr;
  client_id_ = 0;  // 对齐官方 Deactivate: _tfClientId = TF_CLIENTID_NULL
  thread_mgr_ = nullptr;
  activated_ = false;
  LOG("[tsf] 服务停用：全部 sink 注销，上下文已出栈并释放\n");
  return S_OK;
}

STDMETHODIMP RimeTextService::AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) {
  if (pdwCookie == nullptr || punk == nullptr) {
    return E_INVALIDARG;
  }
  if (!IsEqualIID(riid, IID_ITfKeyEventSink)) {
    return E_NOINTERFACE;
  }
  *pdwCookie = sink_cookie_next_++;
  key_sink_advised_ = true;
  LOG("[tsf] ITfSource::AdviseSink(ITfKeyEventSink) 注册握手完成（cookie=%lu）\n",
      static_cast<unsigned long>(*pdwCookie));
  return S_OK;
}

STDMETHODIMP RimeTextService::UnadviseSink(DWORD dwCookie) {
  if (dwCookie == 0) {
    return E_INVALIDARG;
  }
  key_sink_advised_ = false;
  return S_OK;
}

STDMETHODIMP RimeTextService::OnSetFocus(BOOL fForeground) {
  // weasel KeyEventSink.cpp OnSetFocus:
  //   focus -> m_client.FocusIn()
  //   blur  -> m_client.FocusOut() + _AbortComposition()
  // 桌宠激活/停用通知（对齐 M4+ 方案 A）：这是"用户切到/切走 Beautype"的
  // 标准事件（ITfKeyEventSink 已 Advise 成功，OnSetFocus 必被调）。
  LOG("[tsf] OnSetFocus fForeground=%d client_connected=%d session_started=%d\n",
      fForeground ? 1 : 0, client_connected_ ? 1 : 0, session_started_ ? 1 : 0);
  if (fForeground && client_connected_ && session_started_) {
#ifdef _WIN64
    // M6 定案：64 位进程激活**不再自动拉起桌宠**——riv 9.9MB 冷启动在 64 位
    // 宿主（豆包等）下明显卡顿（实测启动→首帧出画约 8 秒），用户已接受
    // 语言栏右键「打开桌宠」手动开。32 位 DLL（WPS 等）无 _WIN64 → 保留自动拉起。
    LOG("[tsf] OnSetFocus(TRUE) x64：跳过自动拉起桌宠（语言栏菜单可手动打开）\n");
#else
    client_.ImeActivated(true);
#endif
  } else if (fForeground) {
    // IPC 还没连：记 pending，等 session_started_=true 后补发（OnSetFocus(TRUE)
    // 只调一次，不补发就丢了——桌宠从不拉起根因 2026-10-15）
    pending_ime_on_focus_ = true;
    pending_focus_in_ = true;  // 2026-10-17：FocusIn 同样必须补发，否则服务端
                               // last_active_session_ 恒 0 → 候选点击不上屏
  } else if (client_connected_ && session_started_) {
    client_.ImeActivated(false);
  }
  if (!fForeground) {
    candidate_window_.Hide();
    if (client_connected_ && session_started_) {
      client_.FocusOut();
      AbortComposition();
      // 2026-10-23 会话维度去重配套：失焦 = 组合中止、服务端会话将重建
      // （重连后 sid 变化），位置基准作废 → 下次聚焦首次必报（防空窗）。
      last_reported_rc_ = {};
      last_reported_session_ = ~0ull;
      LOG("[tsf] 失焦：FocusOut + AbortComposition\n");
    }
  } else if (client_connected_ && session_started_) {
    client_.FocusIn();
  }
  return S_OK;
}

// TSF 同步送达键盘事件，此处用 GetKeyboardState 查询修饰键状态组装 mask
//（对齐官方 weasel ConvertKeyEvent：KeyEvent.cpp:15-28 用 GetKeyboardState
// 取 keyState[VK_SHIFT]&0x80 / keyState[VK_CAPITAL]&0x01 等；GetAsyncKeyState /
// GetKeyState 在 TSF 回调线程拿不到组合键位，真机根因 2026-10-06：Shift+y
// mask=0 打不出大写、CapsLock mask=0 绑不定不触发）
static int CurrentModifierMask() {
  BYTE ks[256] = {0};
  if (!GetKeyboardState(ks)) {
    return 0;
  }
  int mask = 0;
  if (ks[VK_SHIFT] & 0x80) mask |= kKeyMaskShift;
  if (ks[VK_CONTROL] & 0x80) mask |= kKeyMaskControl;
  if (ks[VK_MENU] & 0x80) mask |= kKeyMaskAlt;
  if (ks[VK_CAPITAL] & 0x01) mask |= kKeyMaskCapsLock;
  return mask;
}

STDMETHODIMP RimeTextService::OnTestKeyDown(ITfContext* pContext, WPARAM wParam,
                                            LPARAM lParam, BOOL* pfEaten) {
  LOG("[tsf] >>> OnTestKeyDown vk=0x%02X pending=%d\n", (unsigned)wParam, (int)f_test_key_down_pending_);
  if (pContext != nullptr) {
    context_ = pContext;
  }
  // 对齐小狼毫 KeyEventSink.cpp：TSF 会对同一按键多次调 OnTestKeyDown
  //（MS Word 2010 x64），且部分应用只发 OnKeyDown 不发 OnTestKeyDown
  //（QQ2012）。首个 OnTestKeyDown 实际处理按键并置 pending 位，后续去重；
  // OnKeyDown 时若已置位则直接 pfEaten=TRUE 不重复处理。
  if (pfEaten == nullptr) {
    return E_INVALIDARG;
  }
  if (f_test_key_down_pending_) {
    *pfEaten = TRUE;
    return S_OK;
  }
  *pfEaten = FALSE;
  if (activated_ && client_connected_) {
    HandleRawKeyEx(static_cast<int>(wParam), CurrentModifierMask(), lParam,
                   false, pfEaten);
    if (*pfEaten) {
      f_test_key_down_pending_ = TRUE;
    }
  }
  return S_OK;
}

STDMETHODIMP RimeTextService::OnKeyDown(ITfContext* pContext, WPARAM wParam,
                                        LPARAM lParam, BOOL* pfEaten) {
  if (pContext != nullptr) {
    context_ = pContext;
  }
  if (pfEaten == nullptr) {
    return E_INVALIDARG;
  }
  if (f_test_key_down_pending_) {
    f_test_key_down_pending_ = FALSE;
    *pfEaten = TRUE;
    return S_OK;
  }
  *pfEaten = FALSE;
  if (activated_ && client_connected_) {
    HandleRawKeyEx(static_cast<int>(wParam), CurrentModifierMask(), lParam,
                   false, pfEaten);
  }
  return S_OK;
}

void RimeTextService::HandleRawKey(int vk, int mask, BOOL* pfEaten) {
  // 演示宿主路径：无 lParam（scan=0、非扩展），按按下处理
  HandleRawKeyEx(vk, mask, 0, false, pfEaten);
}

void RimeTextService::HandleRawKeyEx(int vk, int mask, LPARAM lparam,
                                     bool is_up, BOOL* pfEaten) {
  if (!activated_ || !client_connected_) {
    if (pfEaten != nullptr) {
      *pfEaten = FALSE;
    }
    return;
  }
  // 对齐官方 KeyEventSink.cpp:11-17：系统禁用键盘（KEYBOARD_DISABLED /
  // EMPTYCONTEXT compartment）时放行按键，输入法不处理
  if (IsKeyboardDisabled()) {
    if (pfEaten != nullptr) {
      *pfEaten = FALSE;
    }
    return;
  }
  // 官方 KeyInfo（include/KeyEvent.h:3-15）：lParam 位域——bit16-23 扫描码、
  // bit24 扩展键（区分左右 Ctrl/数字键盘 Enter）
  const int scan_code =
      static_cast<int>((static_cast<ULONG_PTR>(lparam) >> 16) & 0xFF);
  const bool extended = (lparam & 0x01000000) != 0;
  int effective_mask = mask;
  if (is_up) {
    // 对齐官方 ConvertKeyEvent（KeyEvent.cpp:27-28）：keyup 时 mask 加
    // RELEASE_MASK（1<<14），服务端 expand_ibus_modifier 展开成 librime
    // kReleaseMask（1<<30）——librime 1.17.0 ascii_composer 的 Shift 短按
    // 切换（500ms 内 release → ToggleAsciiMode）完全依赖此位
    effective_mask |= kKeyMaskRelease;
  }
  // Shift 键：对齐官方（KeyEventSink.cpp:11-63），shift 键直接进引擎
  // （VkToRimeKey 已映射 VK_SHIFT → XK_Shift_L/R），由 librime ascii_composer
  // 的 switch_key 配置处理（default.yaml: Shift_L: commit_code / Shift_R:
  // noop），组合语义由 librime 保证（1.17.0：短按 Shift（500ms 内释放）→
  // ToggleAsciiModeWithKey 切换中英；长按不切）。keydown 清 shift 修饰位发送
  // （本 librime 版本对带 shift 修饰的 Shift_L 不触发 switch_key，实测
  // eaten=0——官方 weasel 内置 librime 能吃，librime 版本差异）；keyup 时
  // shift 已释放，mask 天然无 shift 位。
  if (vk == VK_SHIFT && !is_up) {
    effective_mask &= ~kKeyMaskShift;
  }
  // Ctrl+Space：中英切换（自研快捷键；吃键，不传给应用）。
  // 对齐 weasel：weasel 的切换键在 librime key_binder 配置，我们在 TSF 层
  // 直接拦截等价实现（ToggleAsciiMode → librime ascii_mode）。
  if (!is_up && vk == VK_SPACE && (mask & kKeyMaskControl) != 0 &&
      (mask & kKeyMaskAlt) == 0) {
    ToggleAsciiMode();
    if (pfEaten != nullptr) {
      *pfEaten = TRUE;
    }
    return;
  }
  // F4 方案菜单：服务化后由服务端托盘承担（本地菜单停用，schema_menu_mode_
  // 恒为 false，此分支为死代码但保留结构防误触）
  if (!is_up && schema_menu_mode_) {
    if (vk >= '1' && vk <= '9' || vk == VK_ESCAPE) {
      schema_menu_mode_ = false;
      if (pfEaten != nullptr) *pfEaten = TRUE;
      return;
    }
  }
  // 按键日志：证明键盘事件确实到达服务（真机验收时若候选/上屏失败，
  // 先看此行走没走——没走=焦点/转发问题，走了=引擎链路问题）
  LOG("[tsf] %s按键 0x%02X（VK，mask=%d）\n", is_up ? "释放" : "收到",
      static_cast<unsigned int>(vk), mask);
  // CapsLock 补键 + 官方取消大写锁定模拟（对齐官方 KeyEvent.cpp:30-35 +
  // KeyEventSink.cpp:40-58）：rime 假定 XK_Caps_Lock 在修饰变化之前发送，
  // 而 VK_CAPITAL 收到时系统已切换 caps 状态 → 按下时反转 LOCK 位再进引擎
  // （keyup 不反转，官方 !kinfo.isKeyUp 条件）；caps 键由 librime ascii_composer
  // 的 switch_key Caps_Lock: clear 处理（default.yaml），eaten 由引擎定。
  if (vk == VK_CAPITAL) {
    BOOL eaten_flag = FALSE;
    HandleKey(0xFFE5 /*XK_Caps_Lock*/,
              effective_mask ^ (is_up ? 0 : kKeyMaskCapsLock), &eaten_flag);
    const bool eaten = eaten_flag != FALSE;
    // 官方 KeyEventSink.cpp:40-58：CapsLock 按下被吃（librime 已切中英）+ 本次
    // 是释放 + CapsLock 灯亮 +（已上屏 或 组合中）→ SendInput 模拟再按一次
    // CapsLock 取消大写锁定（否则 caps 亮 → librime 字母直出大写 → 中文
    // 打不出来）。key_count_to_simulate_ 防 SendInput 模拟键递归进入本函数。
    if (prev_key_keycode_ == 0xFFE5 && prev_key_eaten_ &&
        (effective_mask & kKeyMaskRelease) && (key_count_to_simulate_ == 0)) {
      if (GetKeyState(VK_CAPITAL) & 0x01) {
        if (last_key_had_commit_ || (!eaten && ipc_status_.composing)) {
          key_count_to_simulate_ = 2;
          INPUT inputs[2] = {};
          inputs[0].type = INPUT_KEYBOARD;
          inputs[0].ki.wVk = VK_CAPITAL;
          inputs[1].type = INPUT_KEYBOARD;
          inputs[1].ki.wVk = VK_CAPITAL;
          inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
          ::SendInput(2, inputs, sizeof(INPUT));
        }
      }
      if (pfEaten != nullptr) {
        *pfEaten = TRUE;
      }
    }
    if (key_count_to_simulate_ > 0) {
      key_count_to_simulate_--;
    }
    prev_key_keycode_ = 0xFFE5;
    prev_key_eaten_ = eaten;
    return;
  }
  // VK 码 → librime keysym（字母按 shift/caps 取大小写；标点按 shift 位取
  // 上档 keysym；scan/extended 区分左右 Shift/Ctrl，见 VkToRimeKey）
  int keycode =
      VkToRimeKey(vk, (effective_mask & kKeyMaskShift) != 0,
                  (effective_mask & kKeyMaskCapsLock) != 0, scan_code,
                  extended);
  if (keycode == 0 && !is_up) {
    // 官方 ConvertKeyEvent（KeyEvent.cpp:44-55）兜底：未映射 VK 清 Ctrl/Alt
    // 状态后 ToUnicodeEx 转字符（OEM 特殊键等）
    BYTE table[256] = {0};
    if (GetKeyboardState(table)) {
      table[VK_CONTROL] = 0;
      table[VK_MENU] = 0;
      WCHAR buf[8] = {0};
      const int ret = ToUnicodeEx(
          static_cast<UINT>(vk), static_cast<UINT>(lparam & 0xFFFFFFFF), table,
          buf, 8, 0, nullptr);
      if (ret == 1) {
        keycode = static_cast<int>(buf[0]);
      }
    }
  }
  if (keycode == 0) {
    // 无 keysym 映射：不拦截，放行给系统
    if (pfEaten != nullptr) {
      *pfEaten = FALSE;
    }
    return;
  }
  HandleKey(keycode, effective_mask, pfEaten);
  // 记录本次按键（官方 _ProcessKeyEvent 末尾 prevfEaten/prevKeyEvent）
  prev_key_keycode_ = keycode;
  prev_key_eaten_ = (pfEaten != nullptr && *pfEaten != FALSE);
}

STDMETHODIMP RimeTextService::OnTestKeyUp(ITfContext* pContext, WPARAM wParam,
                                          LPARAM lParam, BOOL* pfEaten) {
  // 对齐官方 KeyEventSink.cpp:117-131：先清 down pending（keyup 表示按键周期
  // 结束）；up pending 已置则直接 eat 返回（去重，防 Word 等应用重复发
  // OnTestKeyUp）；否则把 keyup（带 RELEASE 位）发给服务端 + 刷新组合。
  f_test_key_down_pending_ = FALSE;
  if (pfEaten == nullptr) {
    return E_INVALIDARG;
  }
  if (f_test_key_up_pending_) {
    *pfEaten = TRUE;
    return S_OK;
  }
  *pfEaten = FALSE;
  if (activated_ && client_connected_) {
    HandleRawKeyEx(static_cast<int>(wParam), CurrentModifierMask(), lParam,
                   true, pfEaten);
    if (*pfEaten) {
      f_test_key_up_pending_ = TRUE;
    }
  }
  // 官方 OnTestKeyUp 里 _UpdateComposition 无条件调用（组合刷新幂等）
  if (pContext != nullptr && activated_ && client_connected_ &&
      session_started_ && ipc_status_.composing) {
    UpdateCandidateWindow();
  }
  return S_OK;
}

STDMETHODIMP RimeTextService::OnKeyUp(ITfContext* pContext, WPARAM wParam,
                                      LPARAM lParam, BOOL* pfEaten) {
  // 对齐官方 KeyEventSink.cpp:133-147：清 down pending；up pending 已置则清
  // 并 eat（去重）；否则把 keyup（带 RELEASE 位）发给服务端。
  f_test_key_down_pending_ = FALSE;
  if (pfEaten == nullptr) {
    return E_INVALIDARG;
  }
  if (f_test_key_up_pending_) {
    f_test_key_up_pending_ = FALSE;
    *pfEaten = TRUE;
    return S_OK;
  }
  *pfEaten = FALSE;
  if (activated_ && client_connected_) {
    HandleRawKeyEx(static_cast<int>(wParam), CurrentModifierMask(), lParam,
                   true, pfEaten);
  }
  // 组合刷新（官方 OnKeyUp: !_async_edit 时也 _UpdateComposition；组合判断用
  // status.composing（对齐官方 RimeWithWeasel.cpp:761），不用 preedit 非空——
  // 修复前 commit 后服务端不发 ctx，ipc_context_.preedit 残留旧拼音 → 松开
  // 空格误判组合中 → 重新弹出残留拼音（真机"空格上屏后拼音会出来、下次打字
  // 又消失"根因 2026-10-07））
  if (pContext != nullptr && activated_ && client_connected_ &&
      session_started_ && ipc_status_.composing) {
    UpdateCandidateWindow();
  }
  return S_OK;
}

STDMETHODIMP RimeTextService::OnPreservedKey(ITfContext*, REFGUID, BOOL* pfEaten) {
  if (pfEaten != nullptr) {
    *pfEaten = FALSE;
  }
  return S_OK;
}

bool RimeTextService::EnsureSession() {
  // 照官方 WeaselTSF.cpp:239-282 _EnsureServerConnected + _Reconnect：
  // Echo 校验的是"管道已连 + 会话已建"（官方 Echo 要求 session_id != 0，
  // WeaselClientImpl.cpp:169-175），所以连接成功后必须立即 StartSession，
  // 之后 Echo 才可能为真。此前把 Echo 判断放在 StartSession 之前 → 连接
  // 成功但 Echo 恒 false → 永远 FAIL → 打不了中文（真机 log 复现）。
  // 照官方 _EnsureServerConnected（WeaselTSF.cpp:239-282）语义：
  // Connect 失败（服务未建管道）→ Sleep(500) 后重试，共 10 次（≈5 秒窗口，
  // 覆盖首次拉起后 librime 大词库初始化）；attempt==5 且服务进程不在才拉起。
  for (int attempt = 0; attempt < 10; ++attempt) {
    bool ok = client_connected_ && client_.Echo();
    if (!ok) {
      if (client_connected_) {
        client_.Disconnect();
        client_connected_ = false;
        // 2026-10-23 会话维度去重配套：管道断开 = 服务端会话作废，位置基准
        // 同步失效，重连后首次位置必报（防新会话空窗）。
        last_reported_rc_ = {};
        last_reported_session_ = ~0ull;
      }
      if (client_.Connect()) {
        client_connected_ = true;
        // 官方 _Reconnect 顺序（WeaselTSF.cpp:226-235）：Connect 后无条件
        // StartSession（StartSession 内部幂等：_Active() && Echo() 时直接
        // return）。此前用 session_started_ 门控：Disconnect 内部 EndSession
        // 把 session_id 重置为 0，重连后不 StartSession → 后续
        // ProcessKeyEvent 的 _Active() 恒 false → 发送失败（真机 log：
        // “ProcessKeyEvent 发送失败（keycode=0xFF08）”即退格键）。
        client_.StartSession();
        session_started_ = true;
        LOG("[tsf] IPC 会话已启动（pipe=%ls）\n", weasel::GetPipeName().c_str());
        // 补发 OnSetFocus(TRUE) 时 IPC 未连的 ImeActivated（桌宠拉起）；
        // x64 版按 M6 定案跳过自动拉起（见 OnSetFocus 处注释）
        if (pending_ime_on_focus_) {
#ifndef _WIN64
          client_.ImeActivated(true);
#endif
          pending_ime_on_focus_ = false;
          LOG("[tsf] 补发 ImeActivated(true)（OnSetFocus 时 IPC 未连）\n");
        }
        // 2026-10-17：补发 FocusIn（OnSetFocus(TRUE) 时 IPC 未连）——服务端
        // 候选窗点击用 last_active_session_ 归属会话，不补发则恒为 0，点击
        // 候选 SelectCandidateOnCurrentPage(index, 0) 直接 return（不上屏）
        if (pending_focus_in_) {
          client_.FocusIn();
          pending_focus_in_ = false;
          LOG("[tsf] 补发 FocusIn（OnSetFocus 时 IPC 未连）\n");
        }
        // 对齐官方 _Reconnect 230-234：拿一次响应刷新语言栏状态
        if (client_.GetResponseData(std::ref(parser_))) {
          last_ascii_mode_ = ipc_status_.ascii_mode;
          if (lang_bar_ != nullptr) {
            lang_bar_->UpdateAsciiMode(last_ascii_mode_);
          }
          UpdateLanguageBarCompartment();
        }
        ok = true;
      } else {
        // 服务未就绪：照官方每次 Connect 失败 Sleep(500) 后重试
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    }
    if (ok) {
      break;
    }
    if (attempt == 5 && CountRimeServerProcesses() == 0) {
      LOG("[tsf] 服务进程不在，拉起 BeautypeServer.exe\n");
      LaunchRimeServer();
    }
  }
  if (!client_connected_ || !client_.Echo()) {
    LOG("[tsf] FAIL: 无法连接 Beautype 服务进程\n");
    return false;
  }
  return true;
}

bool RimeTextService::SendKeyToServer(int keycode, int mask) {
  if (!EnsureSession()) {
    return false;
  }
  weasel::KeyEvent ke(static_cast<UINT>(keycode), static_cast<UINT>(mask));
  // ProcessKeyEvent 返回值 = 服务端 eaten（官方 WeaselClientImpl.cpp:58-65：
  // ret != 0）。服务端（BeautypeHandler::ProcessKeyEvent 已对齐官方
  // RimeWithWeasel.cpp:264-292）无论 eaten 与否都回响应，所以这里无论返回值
  // 如何都必须 GetResponseData 读掉响应——否则响应泄漏到管道、下一键读到
  // 旧响应 → 重复上屏/状态错乱（真机根因 2026-10-07）。
  const bool eaten = client_.ProcessKeyEvent(ke);
  // v4.7.6：按键转发留痕（观测闭环——DLL 是否真实收到按键并送达服务端）
  LOG("[tsf] key→server vk=0x%02X mask=0x%02X eaten=%d\n",
      static_cast<unsigned int>(keycode), static_cast<unsigned int>(mask),
      eaten ? 1 : 0);
  if (!eaten) {
    LOG("[tsf] 服务端未吃键（keycode=0x%02X mask=0x%02X），仍读响应同步状态\n",
        static_cast<unsigned int>(keycode), static_cast<unsigned int>(mask));
  }
  // 每次响应独立（对齐官方 EditSession.cpp:8-11 每次新建局部 commit）：
  // 解析前清空上次上屏缓存。否则响应无 commit action 时 Committer 不写，
  // last_commit_wide_ 残留上一键的 commit → 后续任意按键（含退格）重复上屏
  // 同一段字、退格被误判为 commit（真机根因 2026-10-07）
  last_commit_wide_.clear();
  if (!client_.GetResponseData(std::ref(parser_))) {
    LOG("[tsf] GetResponseData 解析失败\n");
    return false;
  }
  last_ascii_mode_ = ipc_status_.ascii_mode;
  last_commit_ = ToUtf8(last_commit_wide_);
  // 编码诊断（2026-10-07）：dump 客户端解析出的 commit 宽字符 UTF-16 字节——
  // 中文应为 UTF-16LE（"是"=AF 66）；若为 UTF-8 字节（E6 98 AF）说明客户端
  // 读到的是 UTF-8 字节流（服务端发送编码错位，Word 等应用中文字符乱码）。
  if (::ime::g_log && !last_commit_wide_.empty()) {
    const unsigned char* cb =
        reinterpret_cast<const unsigned char*>(last_commit_wide_.c_str());
    const std::size_t dump_n =
        (last_commit_wide_.size() * sizeof(wchar_t) < 24)
            ? last_commit_wide_.size() * sizeof(wchar_t)
            : 24;
    std::string hex;
    char buf[8];
    for (std::size_t i = 0; i < dump_n; ++i) {
      std::snprintf(buf, sizeof(buf), "%02X ", cb[i]);
      hex += buf;
    }
    LOG("[tsf] commit宽字符字节(dump24)=%s\n", hex.c_str());
  }
  // 每次响应后刷语言栏/输入模式 compartment（对齐官方 EditSession.cpp:16
  // DoEditSession 每次 GetResponseData 后 _UpdateLanguageBar）——否则语言栏
  // 中/英指示不跟随 Shift 切换（真机反馈复现）。注意 _UpdateLanguageBar 既
  // 刷 compartment 也刷语言栏按钮图标（CLangBarItemButton::UpdateWeaselStatus），
  // 本地两者都要做：compartment 驱动系统指示，lang_bar_ 驱动右下角中/A 图标。
  if (lang_bar_ != nullptr) {
    lang_bar_->UpdateAsciiMode(last_ascii_mode_);
  }
  UpdateLanguageBarCompartment();
  return eaten;
}

void RimeTextService::HandleKey(int keycode, int mask, BOOL* pfEaten) {
  if (!EnsureSession()) {
    if (pfEaten != nullptr) {
      *pfEaten = FALSE;
    }
    return;
  }

  // 对齐官方 _committed 语义（KeyEventSink.cpp:44）：记录本次按键处理是否
  // 产生上屏，供 CapsLock 取消大写锁定模拟判断
  last_key_had_commit_ = false;
  const bool eaten = SendKeyToServer(keycode, mask);

  // 对齐官方 EditSession.cpp:6-60：无论 eaten 与否，服务端响应已解析
  // （commit/preedit/status），都要应用到文档（commit 上屏 / 组合刷新）。
  // 修复前 eaten=0 直接 return → DLL 组合缓存与服务端失同步 → 退格不删、
  // 拼音残留重复上屏（真机根因 2026-10-07）。
  if (!last_commit_wide_.empty()) {
    // 有上屏文本：组合存在 → 替换组合文本并结束（标准 TSF 提交）
    last_key_had_commit_ = true;
    // 直接用 last_commit_wide_（UTF-16），不再 ToUtf8 → MultiByteToWideChar 转来转去
    // （两次编码转换可能引入多余空格/控制字符，导致 Chromium 渲染英文时字间距异常宽）
    std::wstring commit_wide = std::move(last_commit_wide_);
    last_commit_wide_.clear();
    last_commit_.clear();
    CommitComposition(std::move(commit_wide));
  } else {
    UpdateCandidateWindow();
  }
  if (pfEaten != nullptr) {
    *pfEaten = eaten ? TRUE : FALSE;
  }
}

bool RimeTextService::ProcessKeySync(int vk, int mask, std::string* commit_out) {
  // 服务化同步路径：按键经官方 Client 转发给服务进程，返回吃没吃。
  // 标准 TSF 语义对照：true=OnKeyDown 返回 S_OK（吃键，不再下发）；
  // false=S_FALSE（放行给系统，焦点窗口原生处理——退格/方向键等编辑键
  // 由此获得原生行为，不再宿主模拟 WM_KEYDOWN 2026-10-04）。
  if (!activated_ || !client_connected_) {
    return false;
  }
  // 英文模式（服务端 ascii_mode=true）：放行给应用原生输入
  if (last_ascii_mode_ && vk >= 0x20 && vk <= 0x7E && vk != VK_BACK) {
    return false;
  }
  // CapsLock：对齐官方进引擎（switch_key Caps_Lock: clear，librime 处理
  // 组合清理 + 模式切换）；eaten 由引擎定
  if (vk == VK_CAPITAL) {
    const int capsmask = mask ^ kKeyMaskCapsLock;
    if (!SendKeyToServer(0xFFE5 /*XK_Caps_Lock*/, capsmask)) {
      return false;
    }
    if (commit_out != nullptr) {
      *commit_out = last_commit_;
    }
    return true;
  }
  const int keycode =
      VkToRimeKey(vk, (mask & kKeyMaskShift) != 0,
                  (mask & kKeyMaskCapsLock) != 0, 0 /*scan*/, false /*ext*/);
  if (keycode == 0) {
    // 无 keysym 映射（修饰键/系统键）：放行给系统
    return false;
  }
  if (!SendKeyToServer(keycode, mask)) {
    return false;
  }
  if (commit_out != nullptr) {
    *commit_out = last_commit_;
  }
  return true;
}

void RimeTextService::ApplyKeyResult() {
  // 主线程应用同步路径（ProcessKeySync）的按键结果：有 commit 上屏，
  // 否则刷新组合/位置；应用后清空缓存防重复上屏。
  if (!activated_) {
    last_commit_.clear();
    return;
  }
  if (!last_commit_.empty()) {
    InsertCommitText(last_commit_);
    last_commit_.clear();
  } else {
    UpdateCandidateWindow();
  }
  last_commit_.clear();
}

void RimeTextService::InsertCommitText(const std::string& commit) {
  if (commit_callback_) {
    commit_callback_(commit);  // 演示宿主回显（真实 TSF 路径不设回调，无副作用）
  }
  if (context_ == nullptr || commit.empty()) {
    return;
  }
  int wlen = MultiByteToWideChar(CP_UTF8, 0, commit.c_str(), -1, nullptr, 0);
  std::wstring wide(wlen > 1 ? static_cast<std::size_t>(wlen - 1) : 0, L'\0');
  if (wlen > 1) {
    MultiByteToWideChar(CP_UTF8, 0, commit.c_str(), -1, &wide[0], wlen);
  }
  // TSF 正确上屏路径：RequestEditSession 拿到本次有效 edit cookie 后，在
  // DoEditSession 内插入（激活时缓存的 cookie 已失效，直接用会失败）。
  CommitEditSession* session =
      new (std::nothrow) CommitEditSession(context_, std::move(wide));
  if (session == nullptr) {
    return;
  }
  HRESULT hrSession = S_OK;
  const HRESULT hr = context_->RequestEditSession(
      client_id_, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
  session->Release();
  LOG("[tsf] 上屏：%s（RequestEditSession 0x%08lX / 会话 0x%08lX）\n",
      commit.c_str(), static_cast<unsigned long>(hr),
      static_cast<unsigned long>(hrSession));
}

void RimeTextService::SetCandidateWindowAnchor(HWND anchor) {
  if (anchor == nullptr) {
    return;
  }
  RECT rc{};
  if (GetWindowRect(anchor, &rc)) {
    candidate_window_.SetPosition(rc.right + 8, rc.top + 8, rc.top);
  }
}

void RimeTextService::UpdateCandidateWindow() {
  // 服务化：候选窗由服务进程（rime_server.exe）自绘弹出，本 DLL 只负责
  // 行内组合（拼音下划线）显示 + 上报输入位置（官方 UpdateInputPosition 语义）。
  const std::wstring preedit = ipc_context_.preedit.str;
  // 2026-10-22 方向键光标修复：从服务端 ctx.preedit.cursor 解析出的 preedit
  // 光标（weasel ResponseParser 把 start,end,cursor 存进
  // preedit.attributes[0].range，官方 Composition.cpp:280-283 同款读取；
  // wchar 前缀长度；-1 = 未下发/末尾语义）
  int preedit_cursor = -1;
  for (const auto& attr : ipc_context_.preedit.attributes) {
    if (attr.type == weasel::HIGHLIGHTED && attr.range.cursor >= 0) {
      preedit_cursor = attr.range.cursor;
      break;
    }
  }
  // 组合判断以服务端 status.composing 为准（对齐官方 RimeWithWeasel.cpp:761
  // "用 RimeStatus.is_composing，不用 preedit 非空推导"）——修复前 commit 后
  // 服务端不发 ctx、ipc_context_.preedit 残留旧拼音 → 误判组合中 → 松键/刷新
  // 时把残留拼音重新弹进文档（真机"空格上屏后拼音又出来"根因 2026-10-07）。
  const bool composing = ipc_status_.composing;
  LOG("[tsf] 组合刷新：composing=%d preedit=%ls cursor=%d\n",
      composing ? 1 : 0, preedit.c_str(), preedit_cursor);
  // 2026-10-10 渲染风暴修复：组合状态与上次完全一致则跳过（不 UpdateComposition
  // 写文档、不上报位置）。OnTestKeyUp/OnKeyUp 每次按键都刷新组合（官方
  // _UpdateComposition 幂等语义），Shift 等不产生新文本的键按下/释放也会触发——
  // 写文档 SetText 会再触发 OnLayoutChange → 上报 → 自激循环（真机单键 20+ 次
  // 相同 rc 上报）。preedit/composing 未变 = 文档无需更新；位置变化由
  // OnLayoutChange → RepositionCandidateWindow 独立路径覆盖。
  // 2026-10-22 补充：preedit 光标纳入幂等比较——方向键只移光标、文本不变，
  // 不比较光标则宿主组合光标永远停在末尾（"左右方向键不能移动拼音光标"）。
  if (composing == last_composing_ && preedit == last_preedit_ &&
      preedit_cursor == last_preedit_cursor_) {
    return;
  }
  last_composing_ = composing;
  last_preedit_ = preedit;
  last_preedit_cursor_ = preedit_cursor;
  if (!composing) {
    // 组合因取消/清空而结束：清空文档里的组合文本（clear=true）
    if (composition_ != nullptr) {
      EndComposition(true);
    }
    return;
  }
  if (context_ == nullptr) {
    return;
  }
  // 行内组合（拼音显示在文档光标处，对齐 weasel CStartComposition/CInlinePreedit）
  if (composition_ == nullptr) {
    StartComposition(context_);
    // 异步 StartComposition 回调（OnCompositionStarted）会再触发定位
    return;
  }
  UpdateComposition(context_, preedit, preedit_cursor);
  // 上报输入位置：服务端候选窗锚定到组合区屏幕坐标。照官方
  // _UpdateCompositionWindow → CGetTextExtentEditSession（Composition.cpp:
  // 213-245）：EditSession 内 GetActiveView + GetTextExt 取组合区范围。
  // 此前用 GetCaretPos+GetForegroundWindow+ClientToScreen：无 caret 应用
  // （浏览器/Electron）返回 (0,0) → 服务端候选窗钉左上角（真机根因）。
  ITfEditSession* pos_session =
      new ServerPositionEditSession(this, context_, composition_);
  HRESULT hrPos = S_OK;
  context_->RequestEditSession(client_id_, pos_session,
                               TF_ES_ASYNCDONTCARE | TF_ES_READ, &hrPos);
  pos_session->Release();
}

void RimeTextService::RepositionCandidateWindow() {
  // 轻量重新定位：只把组合区屏幕坐标上报给服务端候选窗，不写文档文本。
  // OnLayoutChange 里组合文本已是最新（文本由按键路径 UpdateComposition 写入），
  // 布局变化只需候选窗跟随；若在此 UpdateCandidateWindow 会再次 SetText → 再触发
  // OnLayoutChange → 无限自激（2026-10-07 真机卡死：Notepad++ 打字后未响应）。
  if (!activated_ || !client_connected_ || context_ == nullptr ||
      composition_ == nullptr) {
    return;
  }
  ITfEditSession* pos_session =
      new ServerPositionEditSession(this, context_, composition_);
  HRESULT hrPos = S_OK;
  context_->RequestEditSession(client_id_, pos_session,
                               TF_ES_ASYNCDONTCARE | TF_ES_READ, &hrPos);
  pos_session->Release();
}

void RimeTextService::ReportServerPosition(const RECT& rc) {
  if (!activated_ || !client_connected_) {
    return;
  }
  // 2026-10-10 渲染风暴修复：同一位置反复上报直接丢弃（不占管道、不触发服务端
  // SetPosition/渲染）。TSF 布局变化（组合文本每次 SetText 会触发多次
  // OnLayoutChange）单键可产生 20+ 次相同 rc 上报；服务端即使 SetPosition 去重，
  // pipe 写与入队本身也是每键 20+ 次的开销，微软输入法无此自绘架构故不卡。
  // 2026-10-23 会话维度去重：失焦→IPC 重连→服务端重建会话（sid 变化），新会话
  // 首次位置必须上报，否则服务端 haspos=0 → 候选窗空窗。故去重仅在「同会话 &&
  // 位置相同」时生效；会话切换（last_reported_session_ != session_）强制上报并
  // 刷新基准。失焦/IPC 断开处已同步重置基准（见 OnSetFocus/重连代码）。
  if (last_reported_session_ == session_ &&
      rc.left == last_reported_rc_.left && rc.top == last_reported_rc_.top &&
      rc.right == last_reported_rc_.right && rc.bottom == last_reported_rc_.bottom) {
    return;
  }
  const bool session_switched = (last_reported_session_ != session_);
  last_reported_rc_ = rc;
  last_reported_session_ = session_;
  if (::ime::g_log) {
    ::ime::LogF( "[tsf] 上报候选窗位置 rc=(%d,%d)-(%d,%d)%s\n",
                 rc.left, rc.top, rc.right, rc.bottom,
                 session_switched ? "（会话切换强制上报）" : "");
    std::fflush(::ime::g_log);
  }
  client_.UpdateInputPosition(rc);
}

// 托盘/服务端菜单 ID（与服务进程 server_main.cpp 一致）
enum { kTrayAsciiToggle = 1001 };

void RimeTextService::ToggleAsciiMode() {
  // 服务化：中英切换在服务端（SetAsciiModeForAll 全局语义），
  // DLL 经官方 TrayCommand 触发，本地状态立即同步显示。
  if (!activated_ || !EnsureSession()) return;
  client_.TrayCommand(kTrayAsciiToggle);
  last_ascii_mode_ = !last_ascii_mode_;
  if (lang_bar_ != nullptr) {
    lang_bar_->UpdateAsciiMode(last_ascii_mode_);
  }
  UpdateLanguageBarCompartment();
  LOG("[tsf] 中英切换请求：ascii=%d\n", last_ascii_mode_ ? 1 : 0);
}

// ITfCompartmentEventSink：系统中/英指示器、Win+Space 等改变 conversion
// compartment 时同步引擎（对齐 weasel Compartment.cpp）。防回环：读回值
// 与当前 ascii_mode_on_ 一致则不动作（我们 SetCompartmentDWORD 会再触发本回调）。
STDMETHODIMP RimeTextService::OnChange(REFGUID guidCompartment) {
  if (IsEqualGUID(guidCompartment,
                  GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION)) {
    DWORD flags = 0;
    if (GetCompartmentDWORD(flags, guidCompartment)) {
      const bool native = (flags & TF_CONVERSIONMODE_NATIVE) != 0;
      const bool chinese = !last_ascii_mode_;
      if (native != chinese) {
        LOG("[tsf] compartment 变化：native=%d 当前ascii=%d 同步服务端\n",
            native ? 1 : 0, last_ascii_mode_ ? 1 : 0);
        SetAsciiMode(!native);
      }
    }
  }
  return S_OK;
}

bool RimeTextService::InitCompartment() {
  if (thread_mgr_ == nullptr || compartment_sink_cookie_ != TF_INVALID_COOKIE) {
    return false;
  }
  ITfCompartmentMgr* mgr = nullptr;
  if (FAILED(thread_mgr_->QueryInterface(IID_ITfCompartmentMgr,
                                         reinterpret_cast<void**>(&mgr)))) {
    return false;
  }
  ITfCompartment* comp = nullptr;
  if (FAILED(mgr->GetCompartment(
          GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &comp))) {
    mgr->Release();
    return false;
  }
  ITfSource* src = nullptr;
  HRESULT hr = E_NOINTERFACE;
  if (SUCCEEDED(comp->QueryInterface(IID_ITfSource,
                                     reinterpret_cast<void**>(&src)))) {
    hr = src->AdviseSink(IID_ITfCompartmentEventSink,
                         static_cast<ITfCompartmentEventSink*>(this),
                         &compartment_sink_cookie_);
    src->Release();
  }
  comp->Release();
  mgr->Release();
  if (FAILED(hr)) {
    compartment_sink_cookie_ = TF_INVALID_COOKIE;
    return false;
  }
  LOG("[tsf] compartment sink 已注册 cookie=%lu\n",
      static_cast<unsigned long>(compartment_sink_cookie_));
  return true;
}

void RimeTextService::UninitCompartment() {
  if (thread_mgr_ == nullptr ||
      compartment_sink_cookie_ == TF_INVALID_COOKIE) {
    return;
  }
  ITfCompartmentMgr* mgr = nullptr;
  if (SUCCEEDED(thread_mgr_->QueryInterface(
          IID_ITfCompartmentMgr, reinterpret_cast<void**>(&mgr)))) {
    ITfCompartment* comp = nullptr;
    if (SUCCEEDED(mgr->GetCompartment(
            GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &comp))) {
      ITfSource* src = nullptr;
      if (SUCCEEDED(comp->QueryInterface(IID_ITfSource,
                                         reinterpret_cast<void**>(&src)))) {
        src->UnadviseSink(compartment_sink_cookie_);
        src->Release();
      }
      comp->Release();
    }
    mgr->Release();
  }
  compartment_sink_cookie_ = TF_INVALID_COOKIE;
}

void RimeTextService::SetAsciiMode(bool on) {
  if (!activated_ || !EnsureSession()) return;
  if (last_ascii_mode_ == on) return;
  // 服务端全局翻转（官方 TrayCommand → ServerImpl 菜单动作）
  client_.TrayCommand(kTrayAsciiToggle);
  last_ascii_mode_ = on;
  // 语言栏按钮 + 输入模式 compartment 同步（对齐 weasel _UpdateLanguageBar）
  if (lang_bar_ != nullptr) {
    lang_bar_->UpdateAsciiMode(last_ascii_mode_);
  }
  UpdateLanguageBarCompartment();
  LOG("[tsf] ascii_mode set: %s", last_ascii_mode_ ? "EN" : "ZH");
}

void RimeTextService::ShowSchemaMenu() {
  // 服务化后方案切换改由服务端托盘菜单提供（官方架构：schema 切换走
  // 托盘/配置，TSF 端不做本地菜单）；此入口保留为空实现。
  LOG("[tsf] ShowSchemaMenu：服务端托盘菜单承担方案切换\n");
}

// ==================== 行内组合管理（对齐 weasel Composition.cpp） ====================

void RimeTextService::StartComposition(ITfContext* ctx) {
  if (ctx == nullptr || composition_ != nullptr) {
    return;
  }
  StartCompositionEditSession* s =
      new (std::nothrow) StartCompositionEditSession(this, ctx);
  if (s == nullptr) {
    return;
  }
  HRESULT hrSession = S_OK;
  const HRESULT hr = ctx->RequestEditSession(
      client_id_, s, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
  s->Release();
  LOG("[tsf] 启动行内组合（Request 0x%08lX / 会话 0x%08lX）\n",
      static_cast<unsigned long>(hr), static_cast<unsigned long>(hrSession));
}

void RimeTextService::EndComposition(bool clear) {
  ITfContext* ctx = composition_context_;
  ITfComposition* comp = composition_;
  if (ctx == nullptr || comp == nullptr) {
    return;
  }
  EndCompositionEditSession* s =
      new (std::nothrow) EndCompositionEditSession(this, ctx, comp, clear);
  if (s == nullptr) {
    return;
  }
  HRESULT hrSession = S_OK;
  ctx->RequestEditSession(client_id_, s, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                          &hrSession);
  s->Release();
  LOG("[tsf] 结束行内组合（clear=%d，会话 0x%08lX）\n", clear ? 1 : 0,
      static_cast<unsigned long>(hrSession));
  // 立即置空（不等异步回调）
  FinalizeComposition();
}

void RimeTextService::AbortComposition() {
  // 失焦/外部终止/停用：清空组合文本并释放（对齐官方 weasel _AbortComposition）
  // 官方 Composition.cpp _AbortComposition：先经 IPC 通知服务端清空引擎组合
  // （server ClearComposition handler → engine.ClearComposition + UI Hide），
  // 再释放本地 TSF 组合。修复 2026-10-08：此前 OnKillThreadFocus 只做本地
  // 释放、未发 ClearComposition，服务端 composing=1 残留 → 候选窗常驻桌面
  // （真机：打 zhong 未提交切走，候选窗不消失）。
  if (client_connected_ && session_started_) {
    client_.ClearComposition();
  }
  if (composition_ != nullptr) {
    EndComposition(true);
  }
  if (composition_ != nullptr) {  // 兜底：编辑会话失败也强制释放引用
    FinalizeComposition();
  }
}

void RimeTextService::UpdateComposition(ITfContext* ctx,
                                        const std::wstring& preedit,
                                        int sel_cursor) {
  ITfComposition* comp = composition_;
  if (ctx == nullptr || comp == nullptr) {
    return;
  }
  InlinePreeditEditSession* s = new (std::nothrow)
      InlinePreeditEditSession(this, ctx, comp, preedit, sel_cursor);
  if (s == nullptr) {
    return;
  }
  HRESULT hrSession = S_OK;
  ctx->RequestEditSession(client_id_, s, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE,
                          &hrSession);
  s->Release();
}

void RimeTextService::CommitComposition(const std::wstring& commit) {
  if (commit.empty()) {
    return;
  }
  // 组合存在 → 替换组合文本并结束（标准 TSF 提交）；否则直接插入
  if (composition_ != nullptr && composition_context_ != nullptr) {
    ITfComposition* comp = composition_;
    ITfContext* ctx = composition_context_;
    CommitCompositionEditSession* s = new (std::nothrow)
        CommitCompositionEditSession(this, ctx, comp, commit);
    if (s != nullptr) {
      HRESULT hrSession = S_OK;
      ctx->RequestEditSession(
          client_id_, s, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &hrSession);
      s->Release();
      LOG("[tsf] 上屏（组合提交）：%ls（会话 0x%08lX）\n", commit.c_str(),
          static_cast<unsigned long>(hrSession));
      // 立即在同步路径置空（不等异步回调）：下一次按键会 StartComposition
      // 新组合，不会把新拼音写进旧 range 导致 commit 残留 preedit 里。
      // 异步会话里 comp_->EndComposition 仍正常执行（comp 是局部拷贝）。
      FinalizeComposition();
      return;
    }
  }
  // 无组合：直接插入（现有路径）
  std::string utf8;
  int len = WideCharToMultiByte(CP_UTF8, 0, commit.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
  if (len > 1) {
    utf8.resize(static_cast<std::size_t>(len - 1));
    WideCharToMultiByte(CP_UTF8, 0, commit.c_str(), -1, &utf8[0], len, nullptr,
                        nullptr);
  }
  InsertCommitText(utf8);
}

void RimeTextService::SetComposition(ITfComposition* pComposition) {
  composition_ = pComposition;
  composition_context_ = context_;
}

void RimeTextService::OnCompositionStarted(ITfContext* ctx) {
  // weasel CStartCompositionEditSession 末尾：StartComposition 成功后立即定位。
  // 服务化：组合文本 + 候选窗数据来自服务端响应（parser_），此处只负责
  // 行内 preedit 更新与输入位置上报（服务端候选窗跟随）。
  if (ctx == nullptr) {
    return;
  }
  UpdateCandidateWindow();
}

void RimeTextService::FinalizeComposition() {
  composition_ = nullptr;
  composition_context_ = nullptr;
}

void RimeTextService::SetCompositionDisplayAttributes(TfEditCookie ec,
                                                      ITfContext* ctx,
                                                      ITfRange* range) {
  if (display_attribute_guid_atom_ == TF_INVALID_GUIDATOM || range == nullptr) {
    LOG("[tsf] 属性设置跳过: atom=%u range=%p\n",
        display_attribute_guid_atom_, static_cast<void*>(range));
    return;
  }
  BOOL empty = FALSE;
  range->IsEmpty(ec, &empty);
  // 2026-10-14 根因修复：空 range 直接跳过（官方从不对空 range SetValue；空
  // range SetValue 必失败 E_INVALIDARG 且 TSF 留下插入点脏属性段→Chromium
  // 画出"一点点"下划线）。属性只对 SetText 后的全文组合 range 设置。
  if (empty) {
    LOG("[tsf] 属性设置跳过（空 range，防脏属性）\n");
    return;
  }
  ITfProperty* prop = nullptr;
  HRESULT hrProp = ctx->GetProperty(GUID_PROP_ATTRIBUTE, &prop);
  HRESULT hrSet = E_FAIL;
  if (SUCCEEDED(hrProp) && prop != nullptr) {
    VARIANT var = {};
    var.vt = VT_I4;
    var.lVal = static_cast<LONG>(display_attribute_guid_atom_);
    hrSet = prop->SetValue(ec, range, &var);
    prop->Release();
  }
  // 2026-10-14：GUID_PROP_COMPOSING 由 TSF 按组合 range 自动管理，输入法
  // SetValue 实测一律被拒（TF_E_READONLY 0x80040209），不再尝试写入。
  LOG("[tsf] 属性设置 range空=%d atom=%u getprop=0x%08lX setvalue=0x%08lX\n",
      empty ? 1 : 0, display_attribute_guid_atom_,
      static_cast<unsigned long>(hrProp), static_cast<unsigned long>(hrSet));
  // 2026-10-14 诊断：枚举 GUID_PROP_COMPOSING 段，对账 Chromium GetCompositionStatus
  // 的渲染判定（它只对 GUID_PROP_COMPOSING==TRUE 的段画下划线）。验证删除脏属性
  // 后组合段是否单段覆盖全文。
  ITfProperty* propComp = nullptr;
  if (SUCCEEDED(ctx->GetProperty(GUID_PROP_COMPOSING, &propComp)) &&
      propComp != nullptr) {
    IEnumTfRanges* pEnum = nullptr;
    HRESULT hrEnum = propComp->EnumRanges(ec, &pEnum, nullptr);
    if (SUCCEEDED(hrEnum) && pEnum != nullptr) {
      ITfRange* seg = nullptr;
      int segIdx = 0;
      while (pEnum->Next(1, &seg, nullptr) == S_OK) {
        VARIANT vSeg = {};
        HRESULT hrVal = propComp->GetValue(ec, seg, &vSeg);
        const LONG vLong =
            (SUCCEEDED(hrVal) && vSeg.vt == VT_I4) ? vSeg.lVal : -1;
        WCHAR buf[64] = {};
        ULONG cchGot = 0;
        ITfRange* segClone = nullptr;
        if (SUCCEEDED(seg->Clone(&segClone)) && segClone != nullptr) {
          segClone->GetText(ec, 0, buf, 63, &cchGot);
          segClone->Release();
        }
        LOG("[tsf] GUID_PROP_COMPOSING 段%d 值=%ld 文本=%ls(长度%lu)\n",
            segIdx++, vLong, buf, cchGot);
        seg->Release();
      }
      pEnum->Release();
    }
    propComp->Release();
  }
}

void RimeTextService::ClearCompositionDisplayAttributes(TfEditCookie ec,
                                                        ITfContext* ctx,
                                                        ITfRange* range) {
  if (display_attribute_guid_atom_ == TF_INVALID_GUIDATOM || range == nullptr) {
    LOG("[tsf] 属性清除跳过: atom=%u range=%p\n",
        display_attribute_guid_atom_, static_cast<void*>(range));
    return;
  }
  BOOL empty = FALSE;
  range->IsEmpty(ec, &empty);
  ITfProperty* prop = nullptr;
  HRESULT hrProp = ctx->GetProperty(GUID_PROP_ATTRIBUTE, &prop);
  HRESULT hrClear = E_FAIL;
  if (SUCCEEDED(hrProp) && prop != nullptr) {
    hrClear = prop->Clear(ec, range);
    prop->Release();
  }
  // 2026-10-14：GUID_PROP_COMPOSING 不显式清除——由 TSF 按组合 range 自动
  // 管理，EndComposition 本身即清除组合段；输入法 SetValue/Clear 实测一律被拒
  // (TF_E_READONLY 0x80040209)，显式 Clear 既无效也无意义。
  LOG("[tsf] 属性清除 range空=%d atom=%u attr=0x%08lX/0x%08lX\n",
      empty ? 1 : 0, display_attribute_guid_atom_,
      static_cast<unsigned long>(hrProp), static_cast<unsigned long>(hrClear));
}

// ==================== 生命周期 sink（对齐 weasel ActivateEx 顺序） ====================

bool RimeTextService::InitThreadMgrEventSink() {
  if (thread_mgr_ == nullptr) {
    return false;
  }
  ITfSource* source = nullptr;
  if (FAILED(thread_mgr_->QueryInterface(IID_ITfSource,
                                         reinterpret_cast<void**>(&source)))) {
    return false;
  }
  const HRESULT hr = source->AdviseSink(IID_ITfThreadMgrEventSink,
                                        static_cast<ITfThreadMgrEventSink*>(this),
                                        &thread_mgr_event_sink_cookie_);
  source->Release();
  LOG("[tsf] ITfThreadMgrEventSink: 0x%08lX（cookie=%lu）\n",
      static_cast<unsigned long>(hr),
      static_cast<unsigned long>(thread_mgr_event_sink_cookie_));
  return SUCCEEDED(hr);
}

void RimeTextService::UninitThreadMgrEventSink() {
  if (thread_mgr_ == nullptr ||
      thread_mgr_event_sink_cookie_ == TF_INVALID_COOKIE) {
    return;
  }
  ITfSource* source = nullptr;
  if (SUCCEEDED(thread_mgr_->QueryInterface(
          IID_ITfSource, reinterpret_cast<void**>(&source)))) {
    source->UnadviseSink(thread_mgr_event_sink_cookie_);
    source->Release();
  }
  thread_mgr_event_sink_cookie_ = TF_INVALID_COOKIE;
}

bool RimeTextService::InitTextEditSink(ITfDocumentMgr* pDocMgrFocus) {
  if (pDocMgrFocus == nullptr) {
    UninitTextEditSink();
    return false;
  }
  ITfContext* pContext = nullptr;
  if (FAILED(pDocMgrFocus->GetTop(&pContext)) || pContext == nullptr) {
    return false;
  }
  ITfSource* source = nullptr;
  bool ok = false;
  // weasel _InitTextEditSink: 保存当前焦点上下文指针，OnLayoutChange/OnEndEdit
  // 比对（闸门2）——跨文档的布局事件直接忽略
  text_edit_sink_context_ = pContext;
  if (SUCCEEDED(pContext->QueryInterface(IID_ITfSource,
                                         reinterpret_cast<void**>(&source)))) {
    const HRESULT hr = source->AdviseSink(
        IID_ITfTextEditSink, static_cast<ITfTextEditSink*>(this),
        &text_edit_sink_cookie_);
    source->Release();
    ok = SUCCEEDED(hr);
    LOG("[tsf] ITfTextEditSink: 0x%08lX（cookie=%lu）ctx=%p\n",
        static_cast<unsigned long>(hr),
        static_cast<unsigned long>(text_edit_sink_cookie_),
        static_cast<void*>(pContext));
  }
  // 注意：pContext 由本函数 AddRef 持有（GetTop 返回的是带引用的指针），
  // 但这里不 Release——text_edit_sink_context_ 借用它，OnUninit 时再释放。
  return ok;
}

void RimeTextService::UninitTextEditSink() {
  text_edit_sink_context_ = nullptr;
  if (text_edit_sink_cookie_ == TF_INVALID_COOKIE) {
    return;
  }
  ITfDocumentMgr* pDocMgr = nullptr;
  if (thread_mgr_ != nullptr &&
      SUCCEEDED(thread_mgr_->GetFocus(&pDocMgr)) && pDocMgr != nullptr) {
    ITfContext* pContext = nullptr;
    if (SUCCEEDED(pDocMgr->GetTop(&pContext)) && pContext != nullptr) {
      ITfSource* source = nullptr;
      if (SUCCEEDED(pContext->QueryInterface(
              IID_ITfSource, reinterpret_cast<void**>(&source)))) {
        source->UnadviseSink(text_edit_sink_cookie_);
        source->Release();
      }
      pContext->Release();
    }
    pDocMgr->Release();
  }
  text_edit_sink_cookie_ = TF_INVALID_COOKIE;
}

bool RimeTextService::InitTextLayoutSink(ITfDocumentMgr* pDocMgrFocus) {
  if (pDocMgrFocus == nullptr) {
    UninitTextLayoutSink();
    return false;
  }
  ITfContext* pContext = nullptr;
  if (FAILED(pDocMgrFocus->GetTop(&pContext)) || pContext == nullptr) {
    return false;
  }
  ITfSource* source = nullptr;
  bool ok = false;
  if (SUCCEEDED(pContext->QueryInterface(IID_ITfSource,
                                         reinterpret_cast<void**>(&source)))) {
    const HRESULT hr = source->AdviseSink(
        IID_ITfTextLayoutSink, static_cast<ITfTextLayoutSink*>(this),
        &text_layout_sink_cookie_);
    source->Release();
    ok = SUCCEEDED(hr);
    LOG("[tsf] ITfTextLayoutSink: 0x%08lX（cookie=%lu）\n",
        static_cast<unsigned long>(hr),
        static_cast<unsigned long>(text_layout_sink_cookie_));
  }
  pContext->Release();
  return ok;
}

void RimeTextService::UninitTextLayoutSink() {
  if (text_layout_sink_cookie_ == TF_INVALID_COOKIE) {
    return;
  }
  ITfDocumentMgr* pDocMgr = nullptr;
  if (thread_mgr_ != nullptr &&
      SUCCEEDED(thread_mgr_->GetFocus(&pDocMgr)) && pDocMgr != nullptr) {
    ITfContext* pContext = nullptr;
    if (SUCCEEDED(pDocMgr->GetTop(&pContext)) && pContext != nullptr) {
      ITfSource* source = nullptr;
      if (SUCCEEDED(pContext->QueryInterface(
              IID_ITfSource, reinterpret_cast<void**>(&source)))) {
        source->UnadviseSink(text_layout_sink_cookie_);
        source->Release();
      }
      pContext->Release();
    }
    pDocMgr->Release();
  }
  text_layout_sink_cookie_ = TF_INVALID_COOKIE;
}

bool RimeTextService::InitThreadFocusSink() {
  if (thread_mgr_ == nullptr) {
    return false;
  }
  ITfSource* source = nullptr;
  if (FAILED(thread_mgr_->QueryInterface(IID_ITfSource,
                                         reinterpret_cast<void**>(&source)))) {
    return false;
  }
  const HRESULT hr = source->AdviseSink(
      IID_ITfThreadFocusSink, static_cast<ITfThreadFocusSink*>(this),
      &thread_focus_sink_cookie_);
  source->Release();
  LOG("[tsf] ITfThreadFocusSink: 0x%08lX（cookie=%lu）\n",
      static_cast<unsigned long>(hr),
      static_cast<unsigned long>(thread_focus_sink_cookie_));
  return SUCCEEDED(hr);
}

void RimeTextService::UninitThreadFocusSink() {
  if (thread_mgr_ == nullptr ||
      thread_focus_sink_cookie_ == TF_INVALID_COOKIE) {
    return;
  }
  ITfSource* source = nullptr;
  if (SUCCEEDED(thread_mgr_->QueryInterface(
          IID_ITfSource, reinterpret_cast<void**>(&source)))) {
    source->UnadviseSink(thread_focus_sink_cookie_);
    source->Release();
  }
  thread_focus_sink_cookie_ = TF_INVALID_COOKIE;
}

// ==================== ITfActiveLanguageProfileNotifySink ====================

bool RimeTextService::InitActiveLanguageProfileNotifySink() {
  if (thread_mgr_ == nullptr ||
      lang_profile_cookie_ != TF_INVALID_COOKIE) {
    return false;
  }
  // ITfInputProcessorProfiles 是全局对象，经其 ITfSource 注册
  // IID_ITfActiveLanguageProfileNotifySink（对齐官方 weasel ActivateEx 中的
  // _InitActiveLanguageProfileNotifySink 语义：重激活时刷新语言栏状态）
  ITfInputProcessorProfiles* profiles = nullptr;
  if (FAILED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                              CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                              reinterpret_cast<void**>(&profiles))) ||
      profiles == nullptr) {
    LOG("[tsf] FAIL: 获取 ITfInputProcessorProfiles 失败\n");
    return false;
  }
  ITfSource* source = nullptr;
  HRESULT hr = E_NOINTERFACE;
  if (SUCCEEDED(profiles->QueryInterface(
          IID_ITfSource, reinterpret_cast<void**>(&source)))) {
    hr = source->AdviseSink(IID_ITfActiveLanguageProfileNotifySink,
                            static_cast<ITfActiveLanguageProfileNotifySink*>(this),
                            &lang_profile_cookie_);
    source->Release();
  }
  profiles->Release();
  if (FAILED(hr)) {
    lang_profile_cookie_ = TF_INVALID_COOKIE;
    LOG("[tsf] FAIL: AdviseSink(ITfActiveLanguageProfileNotifySink) 0x%08lX\n",
        static_cast<unsigned long>(hr));
    return false;
  }
  LOG("[tsf] 语言档案通知 sink 已注册 cookie=%lu\n",
      static_cast<unsigned long>(lang_profile_cookie_));
  return true;
}

void RimeTextService::UninitActiveLanguageProfileNotifySink() {
  if (thread_mgr_ == nullptr ||
      lang_profile_cookie_ == TF_INVALID_COOKIE) {
    return;
  }
  ITfInputProcessorProfiles* profiles = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_ITfInputProcessorProfiles,
                                 reinterpret_cast<void**>(&profiles))) &&
      profiles != nullptr) {
    ITfSource* source = nullptr;
    if (SUCCEEDED(profiles->QueryInterface(
            IID_ITfSource, reinterpret_cast<void**>(&source)))) {
      source->UnadviseSink(lang_profile_cookie_);
      source->Release();
    }
    profiles->Release();
  }
  lang_profile_cookie_ = TF_INVALID_COOKIE;
}

bool RimeTextService::InitDisplayAttribute() {
  // GUID_PROP_ATTRIBUTE 的属性值是 TfGuidAtom（DWORD），须经
  // ITfCategoryMgr::RegisterGUID 注册取得（微软 TSF Sample 同款机制）。
  // 本机 CoCreateInstance(CLSID_TF_CategoryMgr) 返回 E_NOINTERFACE
  //（register_tip 已验证），统一走 LoadLibraryW("msctf.dll")+TF_CreateCategoryMgr。
  // msctf.dll 为系统常驻 DLL，此处不 FreeLibrary（避免返回对象悬垂）。
  using TF_CreateCategoryMgr_t = HRESULT(WINAPI*)(ITfCategoryMgr**);
  HMODULE h = LoadLibraryW(L"msctf.dll");
  if (h == nullptr) {
    LOG("[tsf] FAIL: 加载 msctf.dll 失败\n");
    return false;
  }
  const auto fn = reinterpret_cast<TF_CreateCategoryMgr_t>(
      GetProcAddress(h, "TF_CreateCategoryMgr"));
  if (fn == nullptr) {
    LOG("[tsf] FAIL: 获取 TF_CreateCategoryMgr 失败\n");
    return false;
  }
  ITfCategoryMgr* mgr = nullptr;
  const HRESULT hr = fn(&mgr);
  if (FAILED(hr) || mgr == nullptr) {
    LOG("[tsf] FAIL: TF_CreateCategoryMgr 0x%08lX\n",
        static_cast<unsigned long>(hr));
    return false;
  }
  const HRESULT hr2 = mgr->RegisterGUID(kGuidCompositionDisplayAttribute,
                                        &display_attribute_guid_atom_);
  mgr->Release();
  LOG("[tsf] 显示属性 RegisterGUID atom=%u（0x%08lX）\n",
      static_cast<unsigned int>(display_attribute_guid_atom_),
      static_cast<unsigned long>(hr2));
  return SUCCEEDED(hr2);
}

void RimeTextService::UninitDisplayAttribute() {
  display_attribute_guid_atom_ = TF_INVALID_GUIDATOM;
}

// ==================== 键盘开关 compartment（GUID_COMPARTMENT_KEYBOARD_OPENCLOSE） ====================

bool RimeTextService::IsKeyboardOpen() {
  if (thread_mgr_ == nullptr) {
    return false;
  }
  ITfCompartmentMgr* mgr = nullptr;
  bool open = false;
  if (SUCCEEDED(thread_mgr_->QueryInterface(
          IID_ITfCompartmentMgr, reinterpret_cast<void**>(&mgr))) &&
      mgr != nullptr) {
    ITfCompartment* compartment = nullptr;
    if (SUCCEEDED(mgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                      &compartment)) &&
        compartment != nullptr) {
      VARIANT var = {};
      if (SUCCEEDED(compartment->GetValue(&var)) && var.vt == VT_I4) {
        open = (var.lVal != 0);
      }
      compartment->Release();
    }
    mgr->Release();
  }
  return open;
}

HRESULT RimeTextService::SetKeyboardOpen(bool open) {
  if (thread_mgr_ == nullptr) {
    return E_FAIL;
  }
  ITfCompartmentMgr* mgr = nullptr;
  HRESULT hr = E_FAIL;
  if (SUCCEEDED(thread_mgr_->QueryInterface(
          IID_ITfCompartmentMgr, reinterpret_cast<void**>(&mgr))) &&
      mgr != nullptr) {
    ITfCompartment* compartment = nullptr;
    if (SUCCEEDED(mgr->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                      &compartment)) &&
        compartment != nullptr) {
      VARIANT var = {};
      var.vt = VT_I4;
      var.lVal = open ? 1 : 0;
      hr = compartment->SetValue(client_id_, &var);
      compartment->Release();
    }
    mgr->Release();
  }
  return hr;
}

// 对齐官方 Compartment.cpp:90-142 _IsKeyboardDisabled：焦点上下文的
// GUID_COMPARTMENT_KEYBOARD_DISABLED 或 GUID_COMPARTMENT_EMPTYCONTEXT 置位
// → 系统禁用键盘（如触屏手写/无障碍），输入法必须放行按键。
bool RimeTextService::IsKeyboardDisabled() {
  if (thread_mgr_ == nullptr) {
    return false;
  }
  ITfDocumentMgr* doc_mgr_focus = nullptr;
  if (FAILED(thread_mgr_->GetFocus(&doc_mgr_focus)) ||
      doc_mgr_focus == nullptr) {
    return true;
  }
  ITfContext* ctx = nullptr;
  if (FAILED(doc_mgr_focus->GetTop(&ctx)) || ctx == nullptr) {
    doc_mgr_focus->Release();
    return true;
  }
  bool disabled = false;
  ITfCompartmentMgr* comp_mgr = nullptr;
  if (SUCCEEDED(ctx->QueryInterface(
          IID_ITfCompartmentMgr, reinterpret_cast<void**>(&comp_mgr)))) {
    ITfCompartment* comp = nullptr;
    if (SUCCEEDED(comp_mgr->GetCompartment(
            GUID_COMPARTMENT_KEYBOARD_DISABLED, &comp))) {
      VARIANT var = {};
      if (SUCCEEDED(comp->GetValue(&var)) && var.vt == VT_I4) {
        disabled = (var.lVal != 0);
      }
      comp->Release();
    }
    if (!disabled) {
      if (SUCCEEDED(comp_mgr->GetCompartment(
              GUID_COMPARTMENT_EMPTYCONTEXT, &comp))) {
        VARIANT var = {};
        if (SUCCEEDED(comp->GetValue(&var)) && var.vt == VT_I4) {
          disabled = (var.lVal != 0);
        }
        comp->Release();
      }
    }
    comp_mgr->Release();
  }
  ctx->Release();
  doc_mgr_focus->Release();
  return disabled;
}

// ==================== ITfThreadMgrEventSink ====================

STDMETHODIMP RimeTextService::OnInitDocumentMgr(ITfDocumentMgr*) {
  return S_OK;
}

STDMETHODIMP RimeTextService::OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr) {
  // 我们的文档管理器被销毁：清场（对齐 weasel 的文档生命周期处理）
  if (pDocMgr == document_mgr_) {
    candidate_window_.Hide();
    AbortComposition();
    LOG("[tsf] 文档管理器销毁：候选窗与组合已清场\n");
  }
  return S_OK;
}

STDMETHODIMP RimeTextService::OnSetFocus(ITfDocumentMgr* pDocMgrFocus,
                                         ITfDocumentMgr*) {
  // 焦点文档切换：重建 TextEdit/TextLayout sink（对齐 weasel _InitTextEditSink）
  if (pDocMgrFocus != nullptr) {
    UninitTextEditSink();
    UninitTextLayoutSink();
    InitTextEditSink(pDocMgrFocus);
    InitTextLayoutSink(pDocMgrFocus);
  }
  return S_OK;
}

STDMETHODIMP RimeTextService::OnPushContext(ITfContext*) {
  return S_OK;
}

STDMETHODIMP RimeTextService::OnPopContext(ITfContext*) {
  return S_OK;
}

// ==================== ITfTextEditSink / ITfTextLayoutSink ====================

STDMETHODIMP RimeTextService::OnEndEdit(ITfContext* pContext,
                                        TfEditCookie, ITfEditRecord* pEditRecord) {
  // weasel TextEditSink.cpp OnEndEdit：不 AbortComposition！只在选区离开
  // 组合 range 时才 EndComposition。此前我们这里一看到外部编辑就 Abort，
  // 导致每次按键（我们自己 SetText 也会走这里）都误杀组合。
  if (composition_ == nullptr || pContext != text_edit_sink_context_) {
    return S_OK;
  }
  // 本服务自己发起的编辑 → 忽略
  BOOL fInWriteSession = FALSE;
  if (SUCCEEDED(pContext->InWriteSession(client_id_, &fInWriteSession)) &&
      fInWriteSession) {
    return S_OK;
  }
  // 检查选区是否移出组合 range（weasel：_CheckComposition）
  if (pEditRecord == nullptr) {
    return S_OK;
  }
  // 轻量判据：选区变化 → 结束组合（不清空引擎状态，交给下一次按键）。
  BOOL fSelectionChanged = FALSE;
  if (SUCCEEDED(pEditRecord->GetSelectionStatus(&fSelectionChanged)) &&
      fSelectionChanged) {
    // 光标走了 → 结束组合（不清空 commit，让引擎自己处理）
    LOG("[tsf] 选区移出组合范围：结束组合\n");
    EndComposition(false);
  }
  return S_OK;
}

STDMETHODIMP RimeTextService::OnLayoutChange(ITfContext* pContext, TfLayoutCode lcode,
                                             ITfContextView*) {
  // weasel TextEditSink.cpp OnLayoutChange 三道闸门：
  // 闸门1: 没在组合 → 直接返回（不 StartComposition、不定位）
  if (composition_ == nullptr) {
    return S_OK;
  }
  // 闸门2: 不是当前 TextEditSink 监听的上下文 → 忽略（防递归/跨文档）
  if (pContext != text_edit_sink_context_) {
    return S_OK;
  }
  // 闸门3: 只做轻量重新定位，不 UpdateCandidateWindow（不 StartComposition/
  // 不改文本）。weasel 这里只 _UpdateCompositionWindow(pContext)。
  if (!activated_ || !client_connected_) {
    return S_OK;
  }
  if (ipc_context_.preedit.str.empty()) {
    return S_OK;
  }
  if (lcode == TF_LC_CHANGE || lcode == TF_LC_CREATE) {
    // 布局变化 → 只重新上报输入位置（服务端候选窗跟随），不写文本。
    // 修复 2026-10-07 卡死根因：此前这里调用 UpdateCandidateWindow() →
    // UpdateComposition() → InlinePreeditEditSession::SetText 写行内拼音 →
    // TSF 文本变化再触发本 OnLayoutChange → 无限自激循环（真机复现：
    // Notepad++ 打字后未响应，日志"组合刷新+上报位置 rc 恒定"无限重复）。
    RepositionCandidateWindow();
  }
  return S_OK;
}

// ==================== ITfThreadFocusSink ====================

STDMETHODIMP RimeTextService::OnSetThreadFocus() {
  // 对齐 weasel OnSetThreadFocus（WeaselTSF.cpp:173-186）的"探测服务并刷新
  // 语言栏状态"意图。官方用 Echo + ProcessKeyEvent(0) 拉真实状态——但该调用
  // 在线程获焦瞬间发起同步 IPC（服务端要同步渲染候选窗 + 写管道），在本地
  // "服务端候选窗 + IPC"架构下与焦点切换竞争，2026-10-07 真机导致服务端
  // SyncUi 卡死、DLL GetResponseData 永久等待、所有应用无响应（已回滚）。
  // 保持官方意图（获焦刷新指示），但只用缓存状态刷新 compartment，不发起
  // 同步 IPC；服务端真实状态以按键响应为准。
  if (client_connected_ && client_.Echo()) {
    UpdateLanguageBarCompartment();
  }
  return S_OK;
}

STDMETHODIMP RimeTextService::OnKillThreadFocus() {
  // 线程失焦：中止组合 + 隐藏候选（对齐 weasel OnKillThreadFocus →
  // _AbortComposition()）
  candidate_window_.Hide();
  AbortComposition();
  return S_OK;
}

// ==================== ITfActiveLanguageProfileNotifySink ====================

STDMETHODIMP RimeTextService::OnActivated(REFCLSID clsid, REFGUID,
                                          BOOL isActivated) {
  // 对齐 weasel OnActivated（WeaselTSF.cpp:209-224）：本服务被激活 →
  // _ShowLanguageBar(TRUE) + _UpdateLanguageBar（语言栏中/英状态刷新，
  // 修复"切走再切回指示消失"）；被停用 → _ShowLanguageBar(FALSE) +
  // _DeleteCandidateList（修复右下角中/英图标时隐时现：官方在停用时隐藏
  // 语言栏按钮，本地此前不控制显隐）。
  if (!IsEqualCLSID(clsid, CLSID_BeautypeTSF)) {
    return S_OK;
  }
  // 注：桌宠激活/停用通知已改挂 ITfKeyEventSink::OnSetFocus（1204 行）——
  // ITfActiveLanguageProfileNotifySink Advise 一直失败 0x80040202，OnActivated
  // 不会被框架调。此处不再发 ImeActivated（防双发）。
  if (isActivated) {
    if (lang_bar_ != nullptr) {
      lang_bar_->Show(TRUE);
    }
    UpdateLanguageBarCompartment();
  } else {
    if (lang_bar_ != nullptr) {
      lang_bar_->Show(FALSE);
    }
    if (candidate_ui_ != nullptr) {
      candidate_ui_->DestroyAll();
    }
  }
  return S_OK;
}

// ==================== ITfCompositionSink ====================

STDMETHODIMP RimeTextService::OnCompositionTerminated(TfEditCookie ec,
                                                       ITfComposition* pComposition) {
  // weasel OnCompositionTerminated:
  //   if (!_IsCurrentComposition(pComposition)) return;
  //   if (_status.composing) { _FinalizeComposition(); return; }
  //   _AbortComposition();
  if (composition_ != nullptr && composition_ == pComposition) {
    // 2026-10-14 下划线/豆包特异诊断：宿主主动终止组合时的完整上下文
    LOG("[tsf] 组合被宿主终止 activated=%d client=%d composing=%d preedit=%ls ctx=%p\n",
        activated_ ? 1 : 0, client_connected_ ? 1 : 0,
        ipc_status_.composing ? 1 : 0, ipc_context_.preedit.str.c_str(),
        static_cast<void*>(context_));
    // 2026-10-14 系统性污染修复：宿主终止组合也必须清理 GUID_PROP_ATTRIBUTE。
    // 此前终止路径只释放组合、不清显示属性 → 样式属性段残留文档上下文 →
    // 切换微软拼音后微软枚举到 Beautype 残留段画出冲突下划线（2026-10-14
    // 用户实测微软拼音被污染）。提交/取消路径已有 Clear（EndCompositionEditSession
    // 无条件清），此处补齐终止路径。
    ITfRange* tr = nullptr;
    if (SUCCEEDED(pComposition->GetRange(&tr)) && tr != nullptr) {
      ITfContext* tctx = nullptr;
      tr->GetContext(&tctx);
      if (tctx != nullptr) {
        ClearCompositionDisplayAttributes(ec, tctx, tr);
        tctx->Release();
      }
      tr->Release();
    }
    // 组合判断用 status.composing（对齐官方 RimeWithWeasel.cpp:761）——
    // 修复前用 preedit 非空，commit 后残留拼音误判组合中，宿主终止时不
    // 清服务端状态，拼音残留在文档（真机根因同 OnKeyUp 2026-10-07）。
    if (activated_ && client_connected_ && ipc_status_.composing) {
      // 服务端还在 composing：只释放 TSF 组合，不清服务端状态（weasel _FinalizeComposition）
      FinalizeComposition();
    } else {
      // 服务端已空：彻底中止
      AbortComposition();
    }
    candidate_window_.Hide();
  }
  return S_OK;
}

// ==================== ITfDisplayAttributeProvider ====================

STDMETHODIMP RimeTextService::EnumDisplayAttributeInfo(
    IEnumTfDisplayAttributeInfo** ppEnum) {
  if (ppEnum == nullptr) {
    return E_INVALIDARG;
  }
  *ppEnum = nullptr;
  return E_NOTIMPL;  // 仅提供单个属性，枚举非必需
}

STDMETHODIMP RimeTextService::GetDisplayAttributeInfo(
    REFGUID guid, ITfDisplayAttributeInfo** ppInfo) {
  if (ppInfo == nullptr) {
    return E_INVALIDARG;
  }
  *ppInfo = nullptr;
  if (IsEqualGUID(guid, kGuidCompositionDisplayAttribute)) {
    DisplayAttributeInfoImpl* info = new (std::nothrow)
        DisplayAttributeInfoImpl(kGuidCompositionDisplayAttribute);
    if (info == nullptr) {
      return E_OUTOFMEMORY;
    }
    *ppInfo = info;
    return S_OK;
  }
  return E_INVALIDARG;
}


// ==================== P2：语言栏 + 状态 compartment（对齐 weasel LanguageBar.cpp） ====================

namespace {
// 本输入法 CLSID（与 tsf_dll.cpp / register_tip.cpp 同值；内部链接避免重复定义）
const CLSID kClsidBeautypeTSF = {
    0xB17E78C8, 0xABAF, 0x4891, {0x84, 0x41, 0x42, 0xFC, 0x45, 0xF4, 0x75, 0xA4}};
}  // namespace

bool RimeTextService::InitLanguageBar() {
  if (thread_mgr_ == nullptr) {
    return false;
  }
  ITfLangBarItemMgr* mgr = nullptr;
  if (FAILED(thread_mgr_->QueryInterface(
          IID_ITfLangBarItemMgr, reinterpret_cast<void**>(&mgr))) ||
      mgr == nullptr) {
    LOG("[tsf] FAIL: 获取 ITfLangBarItemMgr 失败\n");
    return false;
  }
  lang_bar_ = new (std::nothrow) ime::tsf::LanguageBarButton(
      GUID_LBI_INPUTMODE, kClsidBeautypeTSF, thread_mgr_,
      [this](bool target) { SetAsciiMode(target); },
      [this](UINT cmd) {
        // 语言栏菜单命令（对齐官方语言栏 → 托盘同 ID 菜单动作）：
        // 经官方 TrayCommand IPC 发服务端 AddMenuHandler 执行
        if (client_connected_ && cmd != 0) {
          client_.TrayCommand(cmd);
        }
      },
      LaunchPetFromLangBar);
  if (lang_bar_ == nullptr) {
    mgr->Release();
    return false;
  }
  const HRESULT hr = mgr->AddItem(lang_bar_);
  if (FAILED(hr)) {
    LOG("[tsf] FAIL: 语言栏 AddItem 0x%08lX\n", static_cast<unsigned long>(hr));
    delete lang_bar_;
    lang_bar_ = nullptr;
    mgr->Release();
    return false;
  }
  lang_bar_->Show(TRUE);
  lang_bar_->UpdateAsciiMode(last_ascii_mode_);
  mgr->Release();
  LOG("[tsf] 语言栏按钮已注册（GUID_LBI_INPUTMODE）\n");
  return true;
}

void RimeTextService::UninitLanguageBar() {
  if (lang_bar_ == nullptr) {
    return;
  }
  if (thread_mgr_ != nullptr) {
    ITfLangBarItemMgr* mgr = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(
            IID_ITfLangBarItemMgr, reinterpret_cast<void**>(&mgr))) &&
        mgr != nullptr) {
      mgr->RemoveItem(lang_bar_);
      mgr->Release();
    }
  }
  delete lang_bar_;
  lang_bar_ = nullptr;
}

void RimeTextService::UpdateLanguageBarCompartment() {
  // 对齐 weasel _UpdateLanguageBar：写 GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION
  // 的中/英位（TF_CONVERSIONMODE_NATIVE），让系统输入法状态（含任务栏图标）
  // 与 ascii_mode 一致
  DWORD flags = 0;
  GetCompartmentDWORD(flags, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
  if (last_ascii_mode_) {
    flags &= ~TF_CONVERSIONMODE_NATIVE;
  } else {
    flags |= TF_CONVERSIONMODE_NATIVE;
  }
  SetCompartmentDWORD(flags, GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION);
}

bool RimeTextService::GetCompartmentDWORD(DWORD& value, REFGUID guid) {
  if (thread_mgr_ == nullptr) {
    return false;
  }
  ITfCompartmentMgr* mgr = nullptr;
  if (FAILED(thread_mgr_->QueryInterface(
          IID_ITfCompartmentMgr, reinterpret_cast<void**>(&mgr))) ||
      mgr == nullptr) {
    return false;
  }
  ITfCompartment* comp = nullptr;
  const HRESULT hr = mgr->GetCompartment(guid, &comp);
  mgr->Release();
  if (FAILED(hr) || comp == nullptr) {
    return false;
  }
  VARIANT var = {};
  const HRESULT hr2 = comp->GetValue(&var);
  bool ok = false;
  if (SUCCEEDED(hr2) && var.vt == VT_I4) {
    value = static_cast<DWORD>(var.lVal);
    ok = true;
  }
  VariantClear(&var);
  comp->Release();
  return ok;
}

bool RimeTextService::SetCompartmentDWORD(DWORD value, REFGUID guid) {
  if (thread_mgr_ == nullptr) {
    return false;
  }
  ITfCompartmentMgr* mgr = nullptr;
  if (FAILED(thread_mgr_->QueryInterface(
          IID_ITfCompartmentMgr, reinterpret_cast<void**>(&mgr))) ||
      mgr == nullptr) {
    return false;
  }
  ITfCompartment* comp = nullptr;
  const HRESULT hr = mgr->GetCompartment(guid, &comp);
  mgr->Release();
  if (FAILED(hr) || comp == nullptr) {
    return false;
  }
  VARIANT var = {};
  var.vt = VT_I4;
  var.lVal = static_cast<LONG>(value);
  const HRESULT hr2 = comp->SetValue(client_id_, &var);
  comp->Release();
  return SUCCEEDED(hr2);
}

void RimeTextService::ProcessCapsLockKey(int mask) {
  // CapsLock：服务化后由服务端 librime 处理（雾凇配置 good_old_caps_lock /
  // Caps_Lock: noop → 标准大写锁，系统完成切换，DLL 放行）。
  // HandleRawKey 已对 VK_CAPITAL 直接放行，本函数保留为空实现。
  (void)mask;
}

}  // namespace ime
