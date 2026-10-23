// Beautype 正式版托盘总控。
// 右下角状态栏托盘图标（蝴蝶 logo）：
//   - 默认自动启动 Rive 桌宠（pet_preview.exe）
// 右键菜单：
//   打开桌宠   → 启动 pet_preview.exe
//   退出       → 移除托盘图标并退出（不关闭已运行的桌宠）
// 桌宠为 Rive 渲染，工作目录 = exe 所在目录（依赖同目录默认皮肤 riv / skins）。
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kTrayId = 1;
constexpr UINT kCmdPet = 1;
constexpr UINT kCmdExit = 2;

std::wstring g_exe_dir;
HWND g_hwnd = nullptr;
NOTIFYICONDATAW g_nid = {};

// 从 exe 所在目录启动（官方程序依赖同目录 resources / _Data）
bool LaunchExe(const std::wstring& exe_path) {
  if (!std::filesystem::exists(exe_path)) return false;
  STARTUPINFOW si = {};
  PROCESS_INFORMATION pi = {};
  si.cb = sizeof(si);
  std::wstring cmd = L"\"" + exe_path + L"\"";
  const std::wstring work = std::filesystem::path(exe_path).parent_path().wstring();
  if (!CreateProcessW(exe_path.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0,
                      nullptr, work.c_str(), &si, &pi)) {
    return false;
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return true;
}

// 候选路径逐个尝试（先 exe_dir 相对，再已知绝对路径）
bool LaunchFirst(const std::vector<std::wstring>& candidates) {
  for (const auto& c : candidates) {
    if (LaunchExe(c)) return true;
  }
  return false;
}

// 进程是否已在运行（防重复启动）
bool IsRunning(const wchar_t* image_name) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe = {};
  pe.dwSize = sizeof(pe);
  bool found = false;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, image_name) == 0) {
        found = true;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return found;
}

void LaunchPet() {
  // 多开：不再 IsRunning 拦截——已有桌宠时点「打开桌宠」即再开一只
  // （pet_preview 槽位锁分配独立位置/皮肤，§单实例已移除）
  LaunchFirst({g_exe_dir + L"\\pet_preview.exe",
               L"C:\\Users\\tumen\\Documents\\Beautype\\tools\\smoke\\pet_preview.exe"});
}

void ShowMenu() {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, kCmdPet, L"打开桌宠");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kCmdExit, L"退出");
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g_hwnd);
  const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                                  pt.x, pt.y, 0, g_hwnd, nullptr);
  DestroyMenu(menu);
  switch (cmd) {
    case kCmdPet: LaunchPet(); break;
    case kCmdExit:
      Shell_NotifyIconW(NIM_DELETE, &g_nid);
      PostQuitMessage(0);
      break;
    default: break;
  }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case kTrayMsg:
      if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP) ShowMenu();
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
  // 防重复实例：已有托盘在跑则直接退出
  if (IsRunning(L"beautype_launcher.exe")) return 0;

  wchar_t exe_path[MAX_PATH];
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  g_exe_dir = std::filesystem::path(exe_path).parent_path().wstring();

  // 隐藏消息窗口（托盘回调宿主）
  WNDCLASSW wc = {};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.lpszClassName = L"BeautypeTrayWindow";
  RegisterClassW(&wc);
  g_hwnd = CreateWindowExW(0, L"BeautypeTrayWindow", L"Beautype", 0, 0, 0, 0, 0,
                           nullptr, nullptr, hInst, nullptr);
  if (!g_hwnd) return 1;

  // 托盘图标（资源 ID 1 = resource.rc 的 IDI_ICON1）
  HICON icon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
  g_nid = {};
  g_nid.cbSize = sizeof(g_nid);
  g_nid.hWnd = g_hwnd;
  g_nid.uID = kTrayId;
  g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g_nid.uCallbackMessage = kTrayMsg;
  g_nid.hIcon = icon;
  wcscpy_s(g_nid.szTip, L"Beautype 输入法美化");
  Shell_NotifyIconW(NIM_ADD, &g_nid);

  // 默认自动启动 Rive 桌宠（pet_preview.exe）
  LaunchPet();

  // 消息循环
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return 0;
}
