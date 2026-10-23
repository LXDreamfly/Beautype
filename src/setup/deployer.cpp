// BeautypeDeployer.exe —— 对齐官方 WeaselDeployer.exe，独立进程跑部署/设置/同步
//
// 与官方 WeaselDeployer 的对齐关系：
//  - 入口：wWinMain（官方 WeaselDeployer.cpp:13 _tWinMain）
//  - 命令行：/install 首次部署、/deploy 重新部署、/settings 设置面板、/sync 用户同步
//    （官方 WeaselDeployer.cpp:61-100 Run 语义：空参数=设置对话框，/deploy /dict /sync）
//  - IPC 维护模式：官方 Configurator.cpp:135-154 Client.Connect→StartMaintenance→
//    干活→EndMaintenance；本工具照此：weasel::Client 连 server pipe 发 StartMaintenance/
//    EndMaintenance，server 端 BeautypeHandler.StartMaintenance（beautype_handler.cpp:473）
//    暂停输入。server 进程不再自己跑部署/UI，全部由本独立进程承担（hang 隔离）。
//  - 互斥体：WeaselDeployerExclusiveMutex（官方 WeaselDeployer.cpp:43，本 exe 单实例）+
//    BeautypeDeployerMutex（官方 WeaselDeployerMutex，deploy/sync 期间持有，
//    rime_server 检测同名互斥体即推迟部署类操作）。
//  - 设置面板：/settings 跑 ime::ui::RunSettingsDialog（复用 server 现有窗口代码）；
//    theme.yaml 写盘后由 server 文件监听线程换肤（on_apply 不 PostMessage，跨进程无 HWND）。
//
// 崩溃修复 2026-10-12（0xC0000005 根因）：
//  wmain/wWinMain 入口下 CRT 只初始化宽字符 __wargv，窄字符 __argv 保持 NULL；
//  vcpkg glog 编译时定义了 HAVE___ARGV，librime api->setup → SetupLogging →
//  glog InitGoogleLogging → ProgramInvocationShortName 读 __argv[0] → 空指针崩溃。
//  修复与 rime_server 同款（server_main.cpp:379-383）。
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

#include <WeaselIPC.h>

#include "engine/rime_engine.h"
#include "fs_util.h"
#include "log/log.h"
#include "ui/settings_window.h"

namespace {

// 日志（与 rime_server 同款：统一写 ::ime::g_log → deployer.log）
#define LOG(fmt, ...)                                                         \
  do {                                                                        \
    if (::ime::g_log != nullptr) ::ime::LogF(fmt, ##__VA_ARGS__);             \
  } while (0)

// 用户数据目录：%APPDATA%\Beautype\user（与 rime_server UserDataDir 一致）
std::filesystem::path UserDataDir() {
  wchar_t appdata[MAX_PATH] = {};
  GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  return std::filesystem::path(appdata) / L"Beautype" / L"user";
}

// 配置目录：%APPDATA%\Beautype\config（theme.yaml 所在；设置面板读写）
std::filesystem::path ConfigDir() {
  wchar_t appdata[MAX_PATH] = {};
  GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
  return std::filesystem::path(appdata) / L"Beautype" / L"config";
}

// 共享数据目录：exe 同目录 data/rime
std::filesystem::path SharedDataDir() {
  wchar_t exe_path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  return std::filesystem::path(exe_path).parent_path() / L"data" / L"rime";
}

// 部署互斥体：rime_server 检测同名互斥体推迟部署类操作（server_main.cpp:405-412）
const wchar_t* kDeployerMutex = L"BeautypeDeployerMutex";
// 本 exe 单实例互斥体（对齐官方 WeaselDeployer.cpp:43 WeaselDeployerExclusiveMutex）
const wchar_t* kExclusiveMutex = L"BeautypeDeployerExclusiveMutex";

// 收尾：关日志后退出
int Finish(int code) {
  if (::ime::g_log != nullptr) {
    std::fclose(::ime::g_log);
    ::ime::g_log = nullptr;
  }
  return code;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
  // ---- DPI 感知（2026-10-23）：声明 Per-Monitor V2。此前未声明 → 系统对进程
  // 虚拟化，固定像素布局在多档缩放/双屏下缩小错位、GetPixel/GetCursorPos 取色
  // 坐标错位（用户"缩放适配"需求）。声明后取色坐标=物理像素一致；布局按
  // GetDpiForWindow/96 缩放（settings_window.cpp 的 g_dpi_scale）。----
  if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    SetProcessDPIAware();  // 低版本系统回落
  }

  // ---- 崩溃根因修复：补窄字符 __argv（照 server_main.cpp:379-383） ----
  if (__argv == nullptr) {
    static char argv0_buf[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, argv0_buf, MAX_PATH) == 0) {
      argv0_buf[0] = '\0';
    }
    static char* argv_storage[2] = { argv0_buf, nullptr };
    __argc = 1;
    __argv = argv_storage;
  }

  // ---- 参数解析（提前：互斥体策略依赖模式） ----
  const bool installing = lpCmdLine != nullptr && !wcscmp(L"/install", lpCmdLine);
  const bool deploying = lpCmdLine != nullptr && !wcscmp(L"/deploy", lpCmdLine);
  const bool settings = lpCmdLine != nullptr && !wcscmp(L"/settings", lpCmdLine);
  const bool syncing = lpCmdLine != nullptr && !wcscmp(L"/sync", lpCmdLine);

  // ---- 本 exe 单实例（对齐官方 WeaselDeployer.cpp:43）----
  // m1 §31 修复：仅部署类（/install /deploy /sync）持有单实例锁；
  // /settings 不持有——根因：设置窗口常开（用户开着设置面板再点菜单
  // "重新部署/用户数据同步"），旧逻辑无条件持锁导致新 deployer 进程
  // CreateMutex 已存在 → return 1 静默退出 → 菜单"点了没反应"。
  if (installing || deploying || syncing) {
    HANDLE h_exclusive = ::CreateMutexW(nullptr, TRUE, kExclusiveMutex);
    if (h_exclusive == nullptr || ::GetLastError() == ERROR_ALREADY_EXISTS) {
      if (h_exclusive != nullptr) ::CloseHandle(h_exclusive);
      return 1;  // 已有部署在跑，直接退出（无日志目录）
    }
    // 持锁到进程退出（Finish 前无需显式 CloseHandle，进程结束自动释放）
  }

  // ---- 用户目录 + 日志（%APPDATA%\Beautype\deployer.log） ----
  const auto user_dir = UserDataDir();
  const auto app_dir = user_dir.parent_path();  // %APPDATA%\Beautype
  std::error_code ec;
  std::filesystem::create_directories(app_dir, ec);
  std::filesystem::create_directories(user_dir, ec);
  ::ime::g_log = std::fopen((app_dir / "deployer.log").string().c_str(), "w");
  LOG("[deployer] === Beautype Deployer ===\n");

  LOG("[deployer] cmdline=\"%ls\"\n", lpCmdLine != nullptr ? lpCmdLine : L"(null)");
  LOG("[deployer] mode=%s\n",
      installing ? "install" : (deploying ? "deploy"
                              : (settings  ? "settings"
                              : (syncing   ? "sync" : "default"))));

  // ---- /settings：设置面板（对齐官方空参数=Configurator.Run 对话框）----
  // 不拿 DeployerMutex（不改 user 数据）；不连 IPC（只读+写 theme.yaml）。
  // theme.yaml 写盘后由 server 文件监听线程换肤（本进程无 g_tray.tray_hwnd）。
  if (settings) {
    const std::string cfg = ConfigDir().string();
    std::filesystem::create_directories(ConfigDir(), ec);
    ime::ui::RunSettingsDialog(nullptr, cfg, [](const ime::skin::Theme&) {
      // 跨进程：不 PostMessage（无 server HWND）；server 文件监听 theme.yaml 自动换肤。
    });
    LOG("[deployer] 设置面板关闭\n");
    return Finish(0);
  }

  // ---- /install：首次部署（不连 IPC——安装时 server 可能尚未运行） ----
  if (installing) {
    ime::RimeEngine engine;
    const std::string shared_dir = SharedDataDir().string();
    if (!engine.Initialize(shared_dir, user_dir.string(), "rime_ice")) {
      LOG("[deployer] FAIL: 安装部署失败\n");
      return Finish(1);
    }
    LOG("[deployer] 安装部署完成 (librime %s)\n", engine.version().c_str());
    return Finish(0);
  }

  // ---- /deploy /sync：拿 DeployerMutex + IPC 维护模式（对齐官方 Configurator）----
  HANDLE h_mutex = ::CreateMutexW(nullptr, TRUE, kDeployerMutex);
  if (h_mutex == nullptr || ::GetLastError() == ERROR_ALREADY_EXISTS) {
    LOG("[deployer] FAIL: 另一部署操作正在进行（BeautypeDeployerMutex），退出\n");
    if (h_mutex != nullptr) ::CloseHandle(h_mutex);
    return Finish(1);
  }

  // 连 server pipe 进维护模式（官方 Configurator.cpp:135-139）。
  // server 未运行时 Connect 失败 → 直接干活（首次部署/独立修复场景不阻塞）。
  weasel::Client client;
  const bool have_server = client.Connect();
  if (have_server) {
    LOG("[deployer] 已连 server，进入维护模式\n");
    client.StartMaintenance();
  }

  int rc = 0;
  if (deploying) {
    ime::RimeEngine engine;
    const std::string shared_dir = SharedDataDir().string();
    if (!engine.Initialize(shared_dir, user_dir.string(), "rime_ice")) {
      LOG("[deployer] FAIL: 重新部署失败\n");
      rc = 1;
    } else {
      LOG("[deployer] 重新部署完成 (librime %s)\n", engine.version().c_str());
    }
  } else if (syncing) {
    // 用户数据同步（对齐官方 Configurator.cpp:198-237 SyncUserData 语义）：
    // 复制 user 数据到 user/sync/<时间戳>，排除已有 sync 目录防嵌套。
    char stamp[32] = {};
    {
      SYSTEMTIME st{};
      ::GetLocalTime(&st);
      std::snprintf(stamp, sizeof(stamp), "%04d%02d-%02d-%02d%02d%02d",
                    st.wYear, st.wMonth, st.wDay,
                    st.wHour, st.wMinute, st.wSecond);
    }
    const std::string sync_root =
        (user_dir / "sync" / stamp).string();
    bool ok = false;
    try {
      ok = ime::CopyDirectoryRecursive(user_dir.string(), sync_root, {"sync"});
    } catch (...) {
      ok = false;
    }
    LOG("[deployer] 用户数据同步 %s → %s\n", ok ? "OK" : "FAIL",
        sync_root.c_str());
    rc = ok ? 0 : 1;
  }

  // 先关 Mutex，再退维护模式（对齐官方 Configurator.cpp:149-154 顺序）
  ::CloseHandle(h_mutex);
  if (have_server) {
    client.EndMaintenance();
    LOG("[deployer] 维护模式结束，服务恢复\n");
  }
  return Finish(rc);
}
