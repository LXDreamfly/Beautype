// Beautype 服务进程（照官方 WeaselServer 架构）：
//   1. 单实例 + 数据目录/引擎/主题装配（复用已验证的 RimeEngine + rime_ui）
//   2. 官方 WeaselIPC Server（WeaselServerImpl）监听命名管道
//   3. RequestHandler（BeautypeHandler）处理按键/选词/选项
//   4. 右下角托盘（Shell_NotifyIcon）：中英切换/设置/退出
// 候选窗（横版自绘）在本进程内显示，DLL（TSF）只做转发与上屏。
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <string>
#include <thread>  // 异步部署/同步/设置窗线程

#include <windows.h>
#include <tlhelp32.h>  // PetPreviewProcessExists（进程级去重，防桌宠双开）
#include <shellapi.h>
#include <shellscalingapi.h>
#include <dbghelp.h>  // MiniDumpWriteDump（崩溃 dump，照官方 dllmain.cpp:53-64）
#pragma comment(lib, "dbghelp.lib")

// WTL 全局模块对象：官方 WeaselServer.cpp:19 定义 CAppModule _Module;
// WeaselServerImpl::Run() 的 CMessageLoop 依赖它（WTL header-only，无 wtl.lib）。
#include <atlbase.h>
#include <WTL/atlapp.h>
CAppModule _Module;

#include <WeaselIPC.h>
#include <WeaselUtility.h>

#include "engine/rime_engine.h"
#include "log/log.h"
#include "fs_util.h"
#include "skin/theme.h"
#include "skin/skin_applier.h"
#include "server/beautype_handler.h"
#include "server/pet_push_server.h"
#include "ui/candidate_window.h"
#include "updater/updater.h"
// 设置面板已搬到独立进程 BeautypeDeployer.exe（/settings）；server 内不再直接跑
// RunSettingsDialog（hang 隔离），故不再 include settings_window.h。

// 出厂默认桌宠皮肤文件名（统一约定，勿散落硬编码）。
// 以后想换默认动画：把新做好的 .riv 命名为 beautype_pet.riv 替换安装包里的文件即可，
// 无需改代码。改名时全局搜索 kDefaultRivName 同步（server/text_service/pet_preview/install.nsi）。
inline constexpr wchar_t kDefaultRivName[] = L"beautype_pet.riv";

// ---- 崩溃 dump（对齐官方 WeaselTSF dllmain.cpp:53-64）----
// SetUnhandledExceptionFilter + MiniDumpWriteDump → %TEMP%\rime.beautype\<exe>-<时间>.<pid>.dmp
static LONG WINAPI BeautypeUnhandledFilter(EXCEPTION_POINTERS* ep) {
  wchar_t dir[MAX_PATH] = {};
  ExpandEnvironmentStringsW(L"%TEMP%\\rime.beautype", dir, MAX_PATH);
  CreateDirectoryW(dir, nullptr);
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t name[MAX_PATH] = {};
  _snwprintf_s(name, MAX_PATH,
               L"%s\\BeautypeServer-%04d%02d%02d-%02d%02d%02d.%lu.dmp", dir,
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               GetCurrentProcessId());
  HANDLE f = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION info{};
    info.ExceptionPointers = ep;
    info.ThreadId = GetCurrentThreadId();
    info.ClientPointers = TRUE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      MiniDumpNormal, &info, nullptr, nullptr);
    CloseHandle(f);
  }
  return EXCEPTION_EXECUTE_HANDLER;
}

// ---- 托盘菜单 ID（照官方 WeaselServerApp.cpp:50-76 菜单项，砍网页链接类） ----
// 放在 namespace 开头：BuildTrayMenu/TrayWindowProc 等函数定义在其后。
enum {
  IDM_ASCII_TOGGLE = 1001,  // 中英切换
  IDM_SCHEMA = 1002,        // 方案（父菜单，子项 1100+）
  IDM_SETTINGS = 1003,      // 设置面板（settings_window）
  IDM_DEPLOY = 1004,        // 重新部署（异步 + 完成通知）
  IDM_DICT = 1005,          // 字典管理（备份/查看词典）
  IDM_SYNC = 1006,          // 用户数据同步
  IDM_USERCONFIG = 1007,    // 打开用户配置目录
  IDM_LOGDIR = 1008,        // 打开日志目录
  IDM_EXIT = 1009,          // 退出
  IDM_PET_TOP = 1010,       // 置顶桌宠（系统托盘菜单；找回置顶入口：桌宠被遮挡/屏外时拉回）
  IDM_PET_TOP_LANGBAR = 1011, // 置顶桌宠（TSF 语言栏右键菜单，同一置顶逻辑，经 TrayCommand IPC 到达）
  IDM_CHECK_UPDATE = 1012,  // 检查更新（托盘 + TSF 语言栏共用：GitHub Releases 主源 + 镜像兜底）
  IDM_SCHEMA_BASE = 1100,   // 方案子菜单起始（+i = 第 i 个方案）
};
constexpr UINT kTrayMsg = WM_APP + 100;
constexpr UINT kApplyThemeMsg = WM_APP + 101;  // 设置窗保存后：主线程换肤

namespace {

// ---- 系统深色模式检测（照官方 UpdateColorTheme 语义：AppsUseLightTheme=0 → dark） ----
bool IsSystemDarkMode() {
  HKEY key = nullptr;
  DWORD value = 1, size = sizeof(value);
  if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
    ::RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                       reinterpret_cast<LPBYTE>(&value), &size);
    ::RegCloseKey(key);
  }
  return value == 0;
}

// ---- 托盘辅助窗口 ----
// 官方用 SystemTraySDK 的内部窗口处理托盘消息并弹出资源菜单（IDR_MENU_POPUP，
// WeaselTrayIcon.cpp:22-38）；我们用代码构建弹出菜单（官方菜单项同款），
// TrackPopupMenu 的命令经 WM_COMMAND 回 ServerImpl 窗口（OnCommand 分发
// AddMenuHandler，WeaselServerImpl.cpp:110-132）。
// 全局状态（main 内装配后填充）：
struct TrayState {
  HWND server_hwnd = nullptr;            // ServerImpl 窗口（WM_COMMAND 去向）
  HWND tray_hwnd = nullptr;              // 托盘辅助窗口（kApplyThemeMsg 去向）
  weasel::Server* server = nullptr;      // Stop() 用
  ime::server::BeautypeHandler* handler = nullptr;
  ime::RimeEngine* engine = nullptr;
  ime::ui::CandidateWindow* win = nullptr;
  std::string user_dir;                  // 用户目录（explorer 打开用）
  std::string log_path;
  std::string config_dir;                // theme.yaml 目录（换肤重载用）
  bool dark = false;
  NOTIFYICONDATAW nid{};                 // 托盘通知（balloon）用
};
TrayState g_tray;

// 托盘气泡通知（官方 WeaselTrayIcon::ShowBalloon 语义；部署/同步完成提示）
void NotifyBalloon(const wchar_t* title, const wchar_t* text) {
  NOTIFYICONDATAW& nid = g_tray.nid;
  nid.uFlags = NIF_INFO;
  wcscpy_s(nid.szInfoTitle, title);
  wcscpy_s(nid.szInfo, text);
  nid.dwInfoFlags = NIIF_INFO;
  ::Shell_NotifyIconW(NIM_MODIFY, &nid);
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;  // 还原基础标志
}

// 构建托盘弹出菜单（方案子菜单动态：引擎 schema 列表）
HMENU BuildTrayMenu() {
  HMENU menu = ::CreatePopupMenu();
  ::AppendMenuW(menu, MF_STRING, IDM_ASCII_TOGGLE, L"中/英 切换");
  // 方案子菜单（引擎 schema 列表）
  HMENU schema_menu = ::CreatePopupMenu();
  if (g_tray.engine != nullptr) {
    const auto schemas = g_tray.engine->GetSchemaList();
    for (std::size_t i = 0; i < schemas.size() && i < 16; ++i) {
      const std::string& id = schemas[i];
      std::wstring name(id.begin(), id.end());
      ::AppendMenuW(schema_menu, MF_STRING, IDM_SCHEMA_BASE + static_cast<UINT>(i),
                    name.c_str());
    }
  }
  ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(schema_menu), L"方案");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, IDM_DEPLOY, L"重新部署");
  ::AppendMenuW(menu, MF_STRING, IDM_CHECK_UPDATE, L"检查更新");
  ::AppendMenuW(menu, MF_STRING, IDM_DICT, L"词典管理");
  ::AppendMenuW(menu, MF_STRING, IDM_SYNC, L"用户数据同步");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"自定义皮肤");
  ::AppendMenuW(menu, MF_STRING, IDM_USERCONFIG, L"打开用户目录");
  ::AppendMenuW(menu, MF_STRING, IDM_LOGDIR, L"打开日志目录");
  // 置顶桌宠（找回置顶入口 2026-10-20）：桌宠未运行时（窗口类不存在）置灰，
  // 防"点了没反应"；运行中点击 = 置顶 + 屏外回移（AddMenuHandler 实现）。
  const bool pet_running = ::FindWindowW(L"BeautypePetPreview", nullptr) != nullptr;
  ::AppendMenuW(menu, MF_STRING | (pet_running ? 0 : MF_GRAYED), IDM_PET_TOP,
                L"置顶桌宠");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");
  return menu;
}

// 弹输入法菜单（托盘/候选窗右键共用）：光标位置 TrackPopupMenu，
// 命令经 WM_COMMAND 回 ServerImpl 窗口（OnCommand 分发 AddMenuHandler）。
void PopTrayMenu() {
  POINT pt;
  ::GetCursorPos(&pt);
  HMENU menu = BuildTrayMenu();
  HWND owner = g_tray.tray_hwnd != nullptr ? g_tray.tray_hwnd : g_tray.server_hwnd;
  if (owner != nullptr) {
    ::SetForegroundWindow(owner);
    const UINT cmd = static_cast<UINT>(
        ::TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                         pt.x, pt.y, 0, owner, nullptr));
    if (cmd != 0 && g_tray.server_hwnd != nullptr) {
      ::PostMessageW(g_tray.server_hwnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
    }
  }
  ::DestroyMenu(menu);
}

LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == kTrayMsg) {
    switch (LOWORD(lParam)) {
      case WM_RBUTTONUP:
      case NIN_SELECT:
      case WM_LBUTTONUP: {
        // 弹菜单：用光标位置（对齐官方 weasel WeaselTrayIcon，修复菜单钉屏幕左上角）
        PopTrayMenu();
        return 0;
      }
    }
  } else if (msg == kApplyThemeMsg) {
    // 设置窗保存：主线程重载 theme.yaml → 候选窗换肤（手动主题优先）
    if (g_tray.win != nullptr && !g_tray.config_dir.empty()) {
      ime::skin::Theme t = ime::skin::DefaultTheme();
      const auto yaml = std::filesystem::path(g_tray.config_dir) / "theme.yaml";
      if (std::filesystem::exists(yaml) &&
          ime::skin::LoadThemeFromYaml(yaml.string(), &t)) {
        g_tray.win->SetTheme(t);
        if (::ime::g_log != nullptr) {
          ::ime::LogF( "[server] 设置保存：候选窗已换肤（%s）\n",
                       t.name.c_str());
        }
      }
    }
    return 0;
  } else if (msg >= ime::updater::kMsgUpdateFound &&
             msg <= ime::updater::kMsgUpdateDone) {
    // 自动更新流程消息（检查完成/发现新版/下载完成/失败）转发给更新模块
    ime::updater::HandleUpdateMessage(hwnd, msg);
    return 0;
  } else if (msg == WM_SETTINGCHANGE) {
    // 深色模式跟随（AppsUseLightTheme 变化；手动主题优先）
    if (g_tray.win != nullptr && g_tray.dark) {
      const bool dark = IsSystemDarkMode();
      if (dark != g_tray.dark) {
        g_tray.dark = dark;
        ime::skin::Theme t = ime::skin::DefaultTheme();
        const auto yaml = std::filesystem::path(g_tray.config_dir) / "theme.yaml";
        if (std::filesystem::exists(yaml))
          ime::skin::LoadThemeFromYaml(yaml.string(), &t);
        if (dark) {
          t.colors.background = 0xFF2B2B2B;
          t.colors.text = 0xFFE6E6E6;
          t.colors.highlight_text = 0xFFFFFFFF;
          t.colors.highlight_background = 0xFF4A4A4A;
        }
        g_tray.win->SetTheme(t);
        if (::ime::g_log != nullptr) {
          ::ime::LogF( "[server] 深色模式切换：%s\n",
                       dark ? "dark" : "light");
        }
      }
    }
    return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 创建托盘辅助窗口（注册类 + 建窗；消息由 ServerImpl 同一线程的消息循环泵送）
HWND CreateTrayWindow(HINSTANCE hinst) {
  static const wchar_t kTrayWndClass[] = L"BeautypeTrayHost";
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = TrayWindowProc;
  wc.hInstance = hinst;
  wc.lpszClassName = kTrayWndClass;
  ::RegisterClassExW(&wc);
  return ::CreateWindowExW(0, kTrayWndClass, L"", WS_POPUP, 0, 0, 0, 0,
                           nullptr, nullptr, hinst, nullptr);
}

// 日志（仅 %APPDATA%\Beautype\rime_server.log 文件日志；正式版 WIN32 子系统无 stdout）
#define LOG(fmt, ...)                                                         \
  do {                                                                        \
    if (::ime::g_log != nullptr) {                                            \
      ::ime::LogF( fmt, ##__VA_ARGS__);                         \
      std::fflush(::ime::g_log);                                              \
    }                                                                         \
  } while (0)

// ---- 拉起桌宠（M4 集成：装完即用） ----
// 读注册表 InstallDir（NSIS 默认 32 位视图写入 → KEY_WOW64_32KEY 读，
// 对齐 text_service.cpp RimeServerInstallDir 语义）→ 检查 $dir\pet\ 下
// pet_preview.exe + 默认皮肤 riv 存在 → ShellExecuteW 拉起（传 riv 路径参数）。
// 桌宠自带单实例互斥（重复拉起自动退出）+ PushServer 退避重连（乱序启动安全）。
// 失败只记日志不影响输入法（桌宠是附属功能，降级为无桌宠）。
std::wstring BeautypeInstallDirForPet() {
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
  if (r != ERROR_SUCCESS || buf[0] == L'\0') return {};
  std::wstring dir(buf);
  // 去尾部反斜杠/空白（NSIS 写 "$INSTDIR" 无尾部反斜杠；防御手工/旧版残留）
  while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L' ')) dir.pop_back();
  return dir;
}

// 桌宠进程是否已存在（CreateToolhelp32 按进程名匹配 pet_preview.exe）。
// 进程启动远早于窗口创建：连续两次 ImeActivated 时，第二次检查窗口类可能还没
// 建好（FindWindowW 查不到），进程级检查在拉起瞬间即命中，避免"固定开两个桌宠"。
bool PetPreviewProcessExists() {
  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  bool found = false;
  if (::Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"pet_preview.exe") == 0) {
        found = true;
        break;
      }
    } while (::Process32NextW(snap, &pe));
  }
  ::CloseHandle(snap);
  return found;
}

void LaunchPetIfInstalled() {
  // M6：桌宠已在运行（窗口类 BeautypePetPreview 已存在，或 pet_preview.exe 进程
  // 已在）→ 跳过重复拉起。pet_preview 自身也有单实例保护（启动时 FindWindowW
  // 同款检测），但先判断可避免"启动一个进程再秒退"的无谓开销（32 位进程每次激活
  // 都走这里，用户已在运行桌宠时切 WPS 输入法不应有任何额外动作）。
  if (::FindWindowW(L"BeautypePetPreview", nullptr) != nullptr ||
      PetPreviewProcessExists()) {
    LOG("[server] 桌宠已在运行，跳过重复拉起\n");
    return;
  }
  const std::wstring dir = BeautypeInstallDirForPet();
  if (dir.empty()) {
    LOG("[server] 注册表无 Beautype InstallDir，跳过拉起桌宠\n");
    return;
  }
  const std::wstring exe = dir + L"\\pet\\pet_preview.exe";
  const std::wstring riv = dir + L"\\pet\\" + kDefaultRivName;
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES ||
      GetFileAttributesW(riv.c_str()) == INVALID_FILE_ATTRIBUTES) {
    LOG("[server] 桌宠组件缺失（%ls / %ls），跳过拉起（输入法正常，仅无桌宠）\n",
        exe.c_str(), riv.c_str());
    return;
  }
  const std::wstring args = L"\"" + riv + L"\"";
  const HINSTANCE hr = ::ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(),
                                       dir.c_str(), SW_SHOWNORMAL);
  const intptr_t ret = reinterpret_cast<intptr_t>(hr);
  LOG("[server] 拉起桌宠 pet_preview.exe ret=%lld（组件: %ls）\n",
      static_cast<long long>(ret), exe.c_str());
  if (ret <= 32) {
    LOG("[server] 警告: 桌宠拉起失败（ShellExecute 错误码 %lld）\n",
        static_cast<long long>(ret));
  }
}

std::string ToUtf8(const std::filesystem::path& p) {
  const std::wstring ws = p.wstring();
  const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 1 ? static_cast<std::size_t>(n - 1) : 0, '\0');
  if (n > 1)
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, nullptr, nullptr);
  return s;
}

// 用户数据目录：%APPDATA%\Beautype\user（与 TSF DLL 共用同一部署产物）
std::filesystem::path UserDataDir() {
  const char* appdata = std::getenv("APPDATA");
  if (appdata == nullptr || *appdata == '\0')
    return std::filesystem::current_path() / "user";
  return std::filesystem::path(appdata) / "Beautype" / "user";
}

// 共享数据目录（同 demo main：exe 上溯 data/rime → 安装版命中）
std::filesystem::path FindSharedDataDir() {
  if (const wchar_t* env = _wgetenv(L"RIME_SHARED_DATA_DIR"); env != nullptr && *env != L'\0')
    return std::filesystem::path(env);
  const std::filesystem::path kOwn("data/rime");
  wchar_t buf[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, buf, MAX_PATH) > 0) {
    std::filesystem::path dir = std::filesystem::path(buf).parent_path();
    for (int i = 0; i < 6 && !dir.empty(); ++i, dir = dir.parent_path()) {
      const auto own = dir / kOwn;
      if (std::filesystem::is_directory(own))
        return own;
    }
  }
  return kOwn;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpstrCmdLine, int) {
  // 对齐官方 WeaselServer.cpp:65-69：/userdir 打开用户数据文件夹
  // （%APPDATA%\Beautype，含 config/user/skins/日志），开始菜单
  // 【用户文件夹】快捷方式调用（官方 LNKFORUSERFOLDER → WeaselServer.exe /userdir）。
  if (!wcscmp(lpstrCmdLine, L"/userdir")) {
    const auto root = UserDataDir().parent_path();  // %APPDATA%\Beautype（user 的上层）
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    // 官方 explore() 实现（WeaselServerApp.h:25-29）：ShellExecuteW explore + 引号路径
    const std::wstring quoted(L"\"" + root.wstring() + L"\"");
    ::ShellExecuteW(nullptr, L"explore", quoted.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
  }
  // v4.6.5：对齐官方 WeaselServer.cpp 第 87-107 行——/quit 参数通过 IPC 优雅关闭服务。
  // 卸载时先运行 rime_server.exe /quit，服务自己关闭释放文件锁，再卸载 DLL 删除文件。
  bool quit = !wcscmp(lpstrCmdLine, L"/q") || !wcscmp(lpstrCmdLine, L"/quit");
  if (quit) {
    // v4.7.7：移除 v4.7.6 退出留痕日志（/quit 进程写 rime_server.log 追加标记）——
    // 用户确认无需观测留痕，避免卸载时在用户电脑残留垃圾文件；优雅关闭逻辑保留。
    weasel::Client client;
    if (client.Connect()) {  // 连接到正在运行的服务
      client.ShutdownServer();  // 优雅关闭服务
      // 重试最多 10 次，确保服务完全关闭
      int retry = 0;
      while (client.Connect() && retry < 10) {
        client.ShutdownServer();
        retry++;
        Sleep(50);
      }
    }
    return 0;
  }

  // ---- 关键修复：wWinMain 入口下 CRT 只初始化宽字符 __wargv，窄字符 __argv
  // 保持 NULL；而 vcpkg glog 编译时定义了 HAVE___ARGV，
  // ProgramInvocationShortName() 走 const_basename(__argv[0]) 取程序短名，
  // 若不补 __argv，librime api->setup（SetupLogging）里 glog 初始化即
  // 空指针读崩溃（0xC0000005）。手动补一份 ANSI 程序名（与 main 入口语义一致，
  // 官方 WeaselServer 用的 glog 无 HAVE___ARGV 故无此问题）。
  if (__argv == nullptr) {
    static char argv0_buf[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, argv0_buf, MAX_PATH) == 0) {
      argv0_buf[0] = '\0';
    }
    static char* argv_storage[2] = { argv0_buf, nullptr };
    __argc = 1;
    __argv = argv_storage;
  }

  if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
    SetProcessDPIAware();
  // 崩溃 dump（照官方 WeaselTSF dllmain.cpp:64 SetUnhandledExceptionFilter）
  ::SetUnhandledExceptionFilter(BeautypeUnhandledFilter);

  // WTL 模块初始化：官方 WeaselServer.cpp:62 在消息循环前 _Module.Init(NULL, hInstance)，
  // 否则 ServerImpl::Run() 的 CMessageLoop 访问未初始化的消息循环表 → 崩溃。
  _Module.Init(nullptr, ::GetModuleHandleW(nullptr));

  // ---- 单实例（官方同款互斥体） ----
  const std::wstring instance_name = L"(Beautype)Rime-Server-" + getUsername();
  HANDLE h_mutex = ::CreateMutexW(nullptr, FALSE, instance_name.c_str());
  if (::GetLastError() == ERROR_ALREADY_EXISTS) {
    LOG("[server] 已有 Beautype 服务进程在运行，退出");
    return 0;
  }
  // ---- 部署锁检测（照官方 RimeWithWeasel.cpp:513 WeaselDeployerMutex）----
  // 部署器/安装器运行中（写 user 数据）时，本服务推迟部署类操作，避免数据竞争
  HANDLE h_deploy_mutex = ::CreateMutexW(nullptr, TRUE, L"BeautypeDeployerMutex");
  const bool deployer_running =
      (h_deploy_mutex != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS);
  if (h_deploy_mutex != nullptr) ::CloseHandle(h_deploy_mutex);
  if (deployer_running)
    LOG("[server] 部署器正在运行（BeautypeDeployerMutex），延迟部署类操作\n");

  // ---- 目录 + 日志 ----
  const auto user_dir = UserDataDir();
  std::error_code ec;
  std::filesystem::create_directories(user_dir, ec);
  const auto log_path = user_dir.parent_path() / "BeautypeServer.log";
  // 日志轮转：当前文件 >10MB → 删旧 .1 → 改名 .1。磁盘上限≈20MB/份
  // （.1 保留上次历史、下次轮转被删；测试期也能收敛既有大日志存量）
  {
    const auto log_path1 = user_dir.parent_path() / "BeautypeServer.log.1";
    if (std::filesystem::file_size(log_path, ec) > 10LL * 1024 * 1024) {
      std::filesystem::remove(log_path1, ec);
      std::filesystem::rename(log_path, log_path1, ec);
    }
  }
  ::ime::g_log = std::fopen(ToUtf8(log_path).c_str(), "a");
  // 日志级别：默认随构建（测试 0 全量 / 发布 1 INFO 起）；
  // config\log_level 文本文件显式覆盖（排障写 0 重启即全量复现）
  {
    const auto lv_path = user_dir.parent_path() / "config" / "log_level";
    std::ifstream lf(lv_path);
    int lv = -1;
    if (lf >> lv) ::ime::SetLogLevel(lv);
    LOG("[server] 日志级别=%d（%s）\n", ::ime::GetLogLevel(),
        lf ? "config 覆盖" : "默认随构建");
  }
  LOG("[server] Beautype IME server starting\n");

  const std::string shared_data_dir = ToUtf8(FindSharedDataDir());

  // ---- 引擎（雾凇拼音默认，方案切换走引擎） ----
  ime::RimeEngine engine;
  if (!engine.Initialize(shared_data_dir, ToUtf8(user_dir), "rime_ice")) {
    LOG("[server] FAIL: 引擎初始化失败（shared=%s user=%s）\n",
        shared_data_dir.c_str(), ToUtf8(user_dir).c_str());
    return 1;
  }
  LOG("[server] librime %s 就绪（shared=%s）\n", engine.version().c_str(),
      shared_data_dir.c_str());

  // ---- 主题（%APPDATA%\Beautype\config\theme.yaml；缺省落盘默认主题） ----
  const auto config_dir = user_dir.parent_path() / "config";
  std::filesystem::create_directories(config_dir, ec);
  ime::skin::Theme theme = ime::skin::DefaultTheme();
  const auto theme_yaml = config_dir / "theme.yaml";
  if (std::filesystem::exists(theme_yaml) &&
      ime::skin::LoadThemeFromYaml(ToUtf8(theme_yaml), &theme)) {
    LOG("[server] 主题已加载: %s\n", theme.name.c_str());
  } else {
    ime::skin::ApplyThemeToConfig(theme, ToUtf8(config_dir));
  }

  // ---- 2026-10-14 修复（v2.1 收尾）：用户自定义主题优先 ----
  // 用户保存过主题后（设置窗「保存并应用」或「保存配置」→ 落 user_theme.saved 标记）：
  //   ① 自动读取 presets/ 下最新保存的命名配置（用户保存过的最新配置文件）；
  //   ② 深色模式不再覆盖用户颜色（尊重用户保存）。
  // 未自定义（全新安装）仍跟随系统深色（官方 UpdateColorTheme 语义）。
  // 兼容修复前的既有用户数据：即使没有标记，只要
  //   presets/ 存在命名配置，或 theme.yaml 内容与默认主题不同 → 视为用户已自定义。
  bool user_saved = std::filesystem::exists(config_dir / L"user_theme.saved");
  if (!user_saved) {
    const auto presets_dir = config_dir / L"presets";
    if (std::filesystem::exists(presets_dir)) {
      std::error_code pec;
      for (const auto& entry :
           std::filesystem::directory_iterator(presets_dir, pec)) {
        if (pec) break;
        if (entry.is_regular_file(pec) &&
            entry.path().extension() == L".yaml") {
          user_saved = true;
          break;
        }
      }
    }
  }
  if (!user_saved) {
    const ime::skin::Theme def = ime::skin::DefaultTheme();
    // 注意：font.size 不参与比较！v5.x 默认字号调大（24→40）后，若把字号
    // 当"自定义主题"信号，会把从未自定义过的默认用户误判为已自定义 →
    // 禁用系统深色跟随（2026-10-16 用户实测：深色模式候选窗变白框）。
    // 深色模式只覆盖 colors，不影响字号，字号自定义与深色跟随无冲突。
    user_saved = (theme.colors.background != def.colors.background ||
                  theme.colors.text != def.colors.text ||
                  theme.colors.highlight_text != def.colors.highlight_text ||
                  theme.colors.highlight_background !=
                      def.colors.highlight_background ||
                  theme.font.family != def.font.family ||
                  theme.skin.mode != def.skin.mode);
  }
  if (user_saved) {
    std::filesystem::path newest_preset;
    std::filesystem::file_time_type newest_ft{};
    const auto presets_dir = config_dir / L"presets";
    if (std::filesystem::exists(presets_dir)) {
      std::error_code pec;
      for (const auto& entry :
           std::filesystem::directory_iterator(presets_dir, pec)) {
        if (pec) break;
        if (!entry.is_regular_file(pec)) continue;
        if (entry.path().extension() != L".yaml") continue;
        const auto ft = entry.last_write_time(pec);
        if (newest_preset.empty() || ft > newest_ft) {
          newest_preset = entry.path();
          newest_ft = ft;
        }
      }
    }
    if (!newest_preset.empty()) {
      ime::skin::Theme saved = theme;
      if (ime::skin::LoadThemeFromYaml(ToUtf8(newest_preset), &saved)) {
        theme = saved;
        LOG("[server] 自动读取用户最新配置: %s\n",
            newest_preset.filename().string().c_str());
      }
    }
    g_tray.dark = false;
  }

  // ---- 深色模式跟随（官方 UpdateColorTheme 语义：系统深色 → 深色主题） ----
  // 仅当用户未保存过主题时跟随（user_theme.saved 标记存在 = 用户自定义，不覆盖）。
  if (!user_saved) {
    const bool dark = IsSystemDarkMode();
    g_tray.dark = dark;
    if (dark) {
      theme.colors.background = 0xFF2B2B2B;          // 深底
      theme.colors.text = 0xFFE6E6E6;                // 浅字
      theme.colors.highlight_text = 0xFFFFFFFF;
      theme.colors.highlight_background = 0xFF4A4A4A;
      LOG("[server] 深色模式：应用深色主题\n");
    }
  }

  // ---- 候选窗（横版自绘；皮肤图片目录 = 用户 config 目录） ----
  ime::ui::CandidateWindow cand_win;
  cand_win.SetTheme(theme);
  cand_win.SetConfigDir(ToUtf8(config_dir));
  cand_win.SetSchemaTitle("rime_ice");

  // ---- RequestHandler（引擎 + 候选窗联动） ----
  ime::server::BeautypeHandler handler(engine, cand_win);

  // 服务启动显式复位中英全局态（默认中文）：global_ascii_mode_ 虽是成员初始
  // false，但在此明确启动契约——服务常驻期间该态被托盘/语言栏切换翻转后不会
  // 自动回落，用户切过一次英文后每次新建会话都继承英文；每次服务启动必须回到
  // 默认中文（同时防未来加入"启动恢复上次状态"逻辑时行为漂移）。
  handler.set_global_ascii_mode(false);

  // ---- 桌宠事件推送服务器（M3-4）----
  // TCP loopback 随机端口 + token 鉴权 + 端口文件
  // （%LOCALAPPDATA%\rime_beautify\runtime\ime.port.json）；
  // 桌宠连接后，ProcessKeyEvent 把真实按键转发过去（istyping 打字动画 +
  // 按键 HUD）。失败不影响输入法主功能（桌宠降级为无动画）。
  ime::server::PetPushServer pet_push;
  if (!pet_push.Start()) {
    LOG("[server] 警告: 桌宠推送服务器启动失败（桌宠将无打字动画/按键 HUD）\n");
  }
  handler.set_pet_push(&pet_push);
  // M4+：桌宠启动策略 = "首次切到 Beautype 输入法才拉起"（用户指定：输入法装
  // 在语言栏常驻，不搞开机自启软件；切走不隐藏，仅按键联动自然失效）。
  // 启动时不再自动拉起（下方消息循环前的 LaunchPetIfInstalled 调用已移除）；
  // 拉起由 handler.ImeActivated(true) 触发（TSF OnActivated → 管道 → 本回调）。
  handler.set_pet_launcher([]() { LaunchPetIfInstalled(); });
  // 维护期托盘图标（对齐官方 WeaselTrayIcon.cpp:82-90 disabled=true→ShowIcon）：
  // Deployer 经 IPC 发 StartMaintenance → handler.StartMaintenance → 本回调 NIM_ADD；
  // EndMaintenance → NIM_DELETE（默认隐藏态）。server 内不再自己弹 UI/重 IO。
  handler.set_maintenance_listener([](bool on) {
    if (on) {
      ::Shell_NotifyIconW(NIM_ADD, &g_tray.nid);
      LOG("[server] 维护模式：托盘图标短暂显示（Deployer 运行中）\n");
    } else {
      ::Shell_NotifyIconW(NIM_DELETE, &g_tray.nid);
      LOG("[server] 维护结束：托盘图标收回（恢复默认隐藏）\n");
    }
  });
  // 用户右键退出桌宠（bye 帧）→ 标记后本服务生命周期不再自动拉起
  pet_push.SetOnPetBye([&handler]() {
    handler.SetPetUserExited();
    LOG("[server] 用户已主动退出桌宠，本次服务不再自动拉起（重启服务恢复）\n");
  });

  // 候选窗交互：点击选词 / 滚轮翻页（服务端直接驱动引擎）
  // 点击后 SendInput 模拟 VK_SELECT（官方 TSF 同款机制）→ 焦点应用的
  // TSF 拦截器收到 → IPC 回服务端 → 响应回传 DLL 上屏。
  cand_win.SetClickCallback([&handler](int index) {
    handler.SelectCandidateOnCurrentPage(static_cast<size_t>(index),
                                         handler.shown_session());
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki = {VK_SELECT, 0, 0, 0, 0};
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki = {VK_SELECT, 0, KEYEVENTF_KEYUP, 0, 0};
    ::SendInput(2, inputs, sizeof(INPUT));
  });
  cand_win.SetScrollCallback([&handler](bool up) {
    handler.ChangePage(up, handler.shown_session(), nullptr);
  });
  // 候选窗右键：弹出输入法菜单（托盘同款）。WPS 内语言栏不可靠、托盘也常被
  // 用户忽略——候选窗是应用内唯一常驻 Beautype UI，右键必须能出菜单
  // （WPS 右键菜单主诉修复 2026-10-13）。
  cand_win.SetRightClickCallback([]() { PopTrayMenu(); });

  // ---- 官方 IPC Server（WeaselServerImpl：管道 + 消息窗口 + 托盘刷新） ----
  weasel::Server server;
  server.SetRequestHandler(&handler);

  // ---- g_tray 装配（托盘辅助窗口 + 异步动作共用） ----
  g_tray.handler = &handler;
  g_tray.engine = &engine;
  g_tray.win = &cand_win;
  g_tray.user_dir = ToUtf8(user_dir);
  g_tray.log_path = ToUtf8(log_path);

  // ---- 托盘菜单动作（照官方 WeaselServerApp.cpp:50-76 AddMenuHandler 全家） ----
  server.AddMenuHandler(IDM_ASCII_TOGGLE, [&handler]() -> bool {
    // 中英切换：所有会话 ascii_mode 翻转（托盘全局语义）
    const bool now = handler.global_ascii_mode();
    handler.SetAsciiModeForAll(!now);
    handler.set_global_ascii_mode(!now);
    return true;
  });
  // 拉起独立进程 BeautypeDeployer.exe（对齐官方 WeaselServerApp.cpp:50-78
  // execute(WeaselDeployer.exe, args)：设置/部署/同步全在独立进程，server 内无 UI/重 IO，
  // 根除"打开设置面板宿主进程 AppHang"）。维护期图标由 handler.StartMaintenance/
  // EndMaintenance IPC 回调管（对齐官方 disabled→ShowIcon）。
  auto LaunchDeployer = [](const wchar_t* arg) {
    wchar_t exe_path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const std::filesystem::path deployer =
        std::filesystem::path(exe_path).parent_path() / L"BeautypeDeployer.exe";
    if (!std::filesystem::exists(deployer)) {
      LOG("[server] FAIL: BeautypeDeployer.exe 不存在（%ls）\n", deployer.c_str());
      return;
    }
    ::ShellExecuteW(nullptr, L"open", deployer.c_str(), arg, nullptr, SW_SHOWNORMAL);
  };
  // 重新部署：ShellExecute 独立进程 Deployer（部署/维护全在 Deployer 进程）
  server.AddMenuHandler(IDM_DEPLOY, [&]() -> bool {
    LaunchDeployer(L"/deploy");
    return true;
  });
  // 词典管理：打开用户词典目录（官方 DictManagementDialog 的最小可用版；
  // 完整对话框后续接入）
  server.AddMenuHandler(IDM_DICT, []() -> bool {
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe",
                    std::wstring(L"/select,\"").append(
                        std::wstring(g_tray.user_dir.begin(),
                                     g_tray.user_dir.end()))
                        .append(L"\"")
                        .c_str(),
                    nullptr, SW_SHOWNORMAL);
    return true;
  });
  // 用户数据同步：ShellExecute 独立进程 Deployer（复制 user 数据全在 Deployer）
  server.AddMenuHandler(IDM_SYNC, [&]() -> bool {
    LaunchDeployer(L"/sync");
    return true;
  });
  server.AddMenuHandler(IDM_USERCONFIG, []() -> bool {
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe",
                    std::wstring(g_tray.user_dir.begin(), g_tray.user_dir.end())
                        .c_str(),
                    nullptr, SW_SHOWNORMAL);
    return true;
  });
  server.AddMenuHandler(IDM_LOGDIR, []() -> bool {
    // v4.7.10 修复：此前 explorer.exe + 日志文件路径 = explorer 尝试"打开该 .log 文件"
    // → 无关联程序时弹"选择应用打开此 .log 文件"（真机 2026-10-15 用户实测）。
    // 正确语义"打开日志目录"：explorer /select 打开日志所在文件夹（%APPDATA%\Beautype）
    // 并选中 rime_server.log，与"打开用户目录"（user 子目录）明确区分。
    const std::wstring log_path(g_tray.log_path.begin(), g_tray.log_path.end());
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe",
                    std::wstring(L"/select,\"").append(log_path).append(L"\"").c_str(),
                    nullptr, SW_SHOWNORMAL);
    return true;
  });
  // 置顶桌宠（找回置顶入口，2026-10-20）：找到桌宠主窗口 → HWND_TOPMOST 置顶；
  // 窗口中心点不在任一显示器工作区（同 pet_preview.cpp windowCenterOnAnyScreen
  // §48 语义）→ 先回移主屏右下角（SPI_GETWORKAREA - 尺寸 - 20px 边距），防
  // "桌宠在屏外点了没反应"（§48）复现。系统托盘(1010)与语言栏(1011)同一逻辑。
  auto topmost_pet = []() -> bool {
    HWND pet = ::FindWindowW(L"BeautypePetPreview", nullptr);
    if (pet == nullptr) {
      LOG("[server] 置顶桌宠：未找到桌宠窗口（菜单已置灰，正常不可达）\n");
      return true;
    }
    RECT rc{};
    ::GetWindowRect(pet, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    const POINT c{ rc.left + w / 2, rc.top + h / 2 };
    MONITORINFO mi{ sizeof(mi) };
    const bool visible =
        ::GetMonitorInfoW(::MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi) &&
        c.x >= mi.rcWork.left && c.x < mi.rcWork.right &&
        c.y >= mi.rcWork.top && c.y < mi.rcWork.bottom;
    const UINT flags = SWP_NOSIZE | SWP_NOACTIVATE;
    if (!visible) {
      // 屏外 → 回移主屏右下角（对齐 pet_preview 默认右下角语义：工作区 - 尺寸 - 20px）
      RECT work{};
      ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
      constexpr int kPetMargin = 20;
      ::SetWindowPos(pet, HWND_TOPMOST, work.right - w - kPetMargin,
                     work.bottom - h - kPetMargin, 0, 0, flags);
      LOG("[server] 置顶桌宠：窗口在屏外，回移主屏右下角 (%d,%d) 并置顶\n",
          work.right - w - kPetMargin, work.bottom - h - kPetMargin);
    } else {
      ::SetWindowPos(pet, HWND_TOPMOST, 0, 0, 0, 0, flags | SWP_NOMOVE);
      LOG("[server] 置顶桌宠：已置顶（窗口 (%d,%d) %dx%d）\n", rc.left, rc.top, w, h);
    }
    return true;
  };
  server.AddMenuHandler(IDM_PET_TOP, topmost_pet);
  server.AddMenuHandler(IDM_PET_TOP_LANGBAR, topmost_pet);
  // 检查更新（托盘 + 语言栏共用）：异步检查 GitHub Releases，结果回调托盘窗口
  server.AddMenuHandler(IDM_CHECK_UPDATE, [&]() -> bool {
    ime::updater::StartCheckUpdate(g_tray.tray_hwnd);
    return true;
  });
  // 设置面板：ShellExecute 独立进程 Deployer（/settings 开 RunSettingsDialog；
  // 设置窗口在 Deployer 进程，server 内无窗口/消息循环，hang 不传染宿主进程。
  // theme.yaml 写盘后由文件监听线程 PostMessage kApplyThemeMsg 换肤）
  server.AddMenuHandler(IDM_SETTINGS, [&]() -> bool {
    LaunchDeployer(L"/settings");
    return true;
  });
  server.AddMenuHandler(IDM_EXIT, [&server]() -> bool {
    // v4.7.7：移除 v4.7.6 托盘退出留痕 LOG（同 /quit 分支，不留垃圾日志）
    server.Stop();
    return true;
  });
  // 方案切换（官方托盘「方案」子菜单 → SelectSchema 全部会话；
  // 候选窗 schema 标题同步，DLL 状态经下次响应回传）
  {
    const auto schemas = engine.GetSchemaList();
    for (std::size_t i = 0; i < schemas.size() && i < 16; ++i) {
      const std::string sid = schemas[i];
      server.AddMenuHandler(
          IDM_SCHEMA_BASE + static_cast<UINT>(i),
          [&handler, &cand_win, sid]() -> bool {
            handler.SelectSchemaForAll(sid);
            cand_win.SetSchemaTitle(sid);
            LOG("[server] 方案切换 → %s", sid.c_str());
            return true;
          });
    }
  }

  const HWND server_hwnd = server.Start();
  if (server_hwnd == nullptr) {
    LOG("[server] FAIL: 服务窗口创建失败\n");
    engine.Finalize();
    return 1;
  }
  g_tray.server_hwnd = server_hwnd;
  g_tray.server = &server;

  // ---- 右下角托盘（Shell_NotifyIcon；消息由辅助窗口处理 → 弹出菜单） ----
  const HWND tray_hwnd = CreateTrayWindow(GetModuleHandleW(nullptr));
  g_tray.tray_hwnd = tray_hwnd;
  g_tray.config_dir = ToUtf8(config_dir);
  NOTIFYICONDATAW& nid = g_tray.nid;
  nid.cbSize = sizeof(nid);
  nid.hWnd = tray_hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = kTrayMsg;
  // 从同目录加载 Beautype logo（与 exe 同目录部署；LR_LOADFROMFILE 外部资源，免嵌入 rc）
  {
    wchar_t exe_path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::filesystem::path icon_path = std::filesystem::path(exe_path).parent_path() / L"beautype-logo.ico";
    nid.hIcon = reinterpret_cast<HICON>(
        ::LoadImageW(nullptr, icon_path.c_str(), IMAGE_ICON, 0, 0,
                     LR_LOADFROMFILE | LR_DEFAULTSIZE));
    if (nid.hIcon == nullptr) {
      nid.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);  // 兜底
    }
  }
  wcscpy_s(nid.szTip, L"Beautype 输入法");
  // 对齐官方小狼毫：托盘图标默认不显示（WeaselIPCData.h:304 display_tray_icon=false；
  // WeaselTrayIcon.cpp:32-33 Create 后立即 RemoveIcon）。右键菜单入口 = 语言栏按钮
  // 右键（language_bar.cpp OnClick TF_LBI_CLK_RIGHT：中英切换/设置/词典/退出全在）。
  // 历史：2026-10-13 曾以托盘常驻兜底"WPS 语言栏右键无反应"，后实锤该现象是
  // WPS（32 位进程）加载了陈旧的 SysWOW64 beautype_tsf.dll（10-14 07:41 v2.1 版，
  // 打包流程漏编 build-x86），并非语言栏右键本身缺陷——托盘常驻既多余又因安装目录
  // 缺 beautype-logo.ico 兜底成系统默认文档图标，故默认隐藏。
  ::Shell_NotifyIconW(NIM_ADD, &nid);
  ::Shell_NotifyIconW(NIM_DELETE, &nid);  // 默认不显示（官方 display_tray_icon=false）

  // ---- 记录本次运行版本到注册表（LastVersion，供诊断/后续逻辑参考）----
  // 2026-10-23 用户拍板：删除「更新完成」弹窗（MessageBox 多余且阻塞主线程卡顿）；
  // 更新下载安装逻辑本身不受影响，仅完成后不再弹提示。
  {
    wchar_t cur_ver[32] = {};
    ::MultiByteToWideChar(CP_UTF8, 0, BEAUTYPE_VERSION, -1, cur_ver, 32);
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Beautype", 0, nullptr,
                          0, KEY_WRITE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
      ::RegSetValueExW(key, L"LastVersion", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(cur_ver),
                       static_cast<DWORD>((wcslen(cur_ver) + 1) *
                                          sizeof(wchar_t)));
      ::RegCloseKey(key);
    }
  }

  // ---- theme.yaml 文件监听线程（换肤生效：设置面板在 Deployer 进程写盘，
  // server 靠文件变化 PostMessage kApplyThemeMsg 触发主线程换肤）----
  // 现有 kApplyThemeMsg 处理（TrayWindowProc:189-203）原样复用，监听线程只负责
  // "发现文件变了 → PostMessage"，不做实际换肤。
  std::thread([]() {
    if (g_tray.config_dir.empty()) return;
    std::wstring dir(g_tray.config_dir.begin(), g_tray.config_dir.end());
    HANDLE h = ::FindFirstChangeNotificationW(
        dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (h == INVALID_HANDLE_VALUE) return;
    while (::WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0) {
      if (g_tray.tray_hwnd != nullptr)
        ::PostMessageW(g_tray.tray_hwnd, kApplyThemeMsg, 0, 0);
      ::FindNextChangeNotification(h);
    }
  }).detach();

  LOG("[server] 托盘已就绪，等待 DLL 连接（pipe=%ls）\n", weasel::GetPipeName().c_str());

  // ---- 主线程预创建候选窗窗口（对齐官方 WeaselServerApp.cpp:30 m_ui.Create()）----
  // 候选窗窗口（含 role 月亮浮层）必须在主线程创建：主线程随后进入 Server::Run()
  // 的 CMessageLoop 消息泵，跨线程窗口操作（pipe 线程 Show/Hide/SetPosition）由
  // 主线程处理。若窗口懒创建在 pipe 工作线程（无消息泵、阻塞读管道），鼠标悬停
  // 候选窗 → 消息无人处理 → 沙漏假死；多宿主并发时跨线程 ShowWindow SendMessage
  // 等一个阻塞读管道的线程 → 死锁（真机根因 2026-10-07：悬停月亮 → 沙漏 → 输入框卡死）。
  cand_win.CreateMainWindow();
  LOG("[server] 候选窗窗口已在主线程创建\n");

  // 【M4+ 启动策略变更】不再在此自动拉起桌宠：开机只起输入法服务（语言栏常驻），
  // 桌宠等用户第一次切到 Beautype 输入法时由 handler.ImeActivated(true) 拉起
  // （见上方 set_pet_launcher 注入）。原 LaunchPetIfInstalled() 调用点已移除。

  // 消息循环（Server::Run 内部 WTL CMessageLoop；托盘消息在此分发）
  const int rc = server.Run();

  ::Shell_NotifyIconW(NIM_DELETE, &nid);
  ::CloseHandle(h_mutex);
  engine.Finalize();
  LOG("[server] 退出 rc=%d\n", rc);
  if (::ime::g_log != nullptr) {
    std::fclose(::ime::g_log);
    ::ime::g_log = nullptr;
  }
  return rc;
}
