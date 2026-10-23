// register_tip.exe —— Beautype TSF 输入法系统注册工具
//
// 完全照小狼毫（weasel）官方源码实现，执行顺序与官方一致：
//  WeaselSetup/imesetup.cpp install_ime_file + register_text_service：
//   [0] 复制 rime_tsf.dll 到 System32\beautype_tsf.dll（照官方第 163 行 copy_file）
//   [0] ShellExecuteExW 调用 regsvr32.exe /s beautype_tsf.dll（照官方第 353-365 行）
//  WeaselTSF/Register.cpp：
//   A. ITfInputProcessorProfileMgr::RegisterProfile（照官方 RegisterProfiles()：
//      注册简体中文 profile，带图标，enable=TRUE）。
//   B. ITfCategoryMgr::RegisterCategory（照官方 RegisterCategories()：

// 改成 GUI 程序（不闪黑窗口），对齐官方 WeaselSetup.exe
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:wmainCRTStartup")
//      注册全部 16 个支持类别 GUID，含键盘输入法必需的 GUID_TFCAT_TIP_KEYBOARD）。
//      ※ 创建 ITfCategoryMgr 改用官方 msctf.dll 导出函数 TF_CreateCategoryMgr()，
//        不经过 COM 类工厂（CoCreateInstance(CLSID_TF_CategoryMgr) 在本机返回
//        E_NOINTERFACE，改用系统导出函数更可靠）。
//  WeaselSetup/imesetup.cpp：
//   C. ITfInputProcessorProfiles::EnableLanguageProfile /
//      EnableLanguageProfileByDefault（照官方 enable_profile(TRUE)）。
//   D. InstallLayoutOrTip（input.dll）：把输入法安装进系统布局，
//      Win11 "设置→语言→添加键盘" 列表从此可见。标题格式照官方
//      profile_to_title()："%04X:{CLSID}{PROFILEGUID}"。
//
// 用法：以管理员身份运行（HKLM 写入）。
//   register_tip.exe          安装/注册（A~E 全部步骤）
//   register_tip.exe /u       卸载（反注册 + 清理注册表）
//
// 2026-10-12 v4.7.0：完全照官方 WeaselSetup 架构——register_tip 改 x86（Win32）
// 构建（官方 WeaselSetup.vcxproj 仅 Win32 平台），InstallDll/UninstallDll 照
// 官方 imesetup.cpp:141-293 的 32 位进程视角：GetSystemDirectoryW（x86 进程被
// WOW64 文件系统重定向到 SysWOW64）装/卸 32 位 DLL；is_wow64() 时
// Wow64DisableWow64FsRedirection 装/卸真实 System32 的 64 位 DLL。CLSID 注册/
// 反注册回归 regsvr32（DllRegisterServer/DllUnregisterServer，官方同款）。
// 历史勘误：v4.6.x 曾因 regsvr32 卡死（实测 CPU≈0 且 CLSID 键未创建）改用 [E]
// 直接写键；该卡死系 regsvr32 位数与 DLL 不匹配弹窗等待所致（官方 0.17.4 安装包
// 经 Get-AuthenticodeSignature 验签 NotSigned——官方无代码签名、官方 regsvr32
// 流程从不卡），x86 架构 + regsvr32 位数匹配后回归官方流程。
//
// 2026-10-11 v4.6.6：对齐官方 install_ime_file——register_tip.exe 内部用
// ShellExecuteExW 调用 regsvr32（不是安装脚本直接调用）。
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <shellapi.h>

// Beautype TSF 身份（与 src/ime/tsf/tsf_dll.cpp 中 DEFINE_GUID 一致）
static const GUID kClsidBeautype = {
    0xB17E78C8, 0xABAF, 0x4891, {0x84, 0x41, 0x42, 0xFC, 0x45, 0xF4, 0x75, 0xA4}};
static const GUID kProfileBeautype = {
    0xFC89E162, 0x09DE, 0x4842, {0xA7, 0xC7, 0x7F, 0x9B, 0xB7, 0x3E, 0x88, 0x8E}};

typedef BOOL(WINAPI* PTF_INSTALLLAYOUTORTIP)(LPCWSTR psz, DWORD dwFlags);
#define ILOT_UNINSTALL 0x00000001
// TF_CreateCategoryMgr：msctf.dll 官方导出（msctf.h 声明）。动态加载避免链接 msctf.lib
// （MSVC 链接器默认库路径不含 Windows SDK 的 msctf.lib，LNK1181）。
typedef HRESULT(WINAPI* PTF_CREATECATEGORYMGR)(ITfCategoryMgr** ppcat);

// 输出同时写控制台与 exe 同目录 register_tip.log（提权运行时 stdout 不可见）
// v4.7.9：落盘改显式 UTF-8（§16.16 教训：禁 _wfopen ccs=UTF-8，会静默失败）——
// 旧实现 _wfopen(L"w")+vfwprintf 在进程默认 "C" locale 下遇 CJK 宽字符转换失败，
// 该 Log 调用从首个 CJK 起整段截断（err 码、换行全丢）→ 决定性诊断字段（复制失败
// 的 Windows err 码）从未写进 log（M1 复盘 §28，真机 2026-10-15 双机同症状）。
static FILE* g_log = nullptr;
static void Log(const wchar_t* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vwprintf(fmt, args);  // 控制台（GUI 子系统通常无输出，保留无妨）
  if (g_log != nullptr) {
    va_list args2;
    va_copy(args2, args);
    wchar_t buf[4096];
    const int n = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args2);
    va_end(args2);
    if (n > 0) {
      char utf8[8192];
      const int cb = WideCharToMultiByte(CP_UTF8, 0, buf, n, utf8,
                                         static_cast<int>(sizeof(utf8)),
                                         nullptr, nullptr);
      if (cb > 0) fwrite(utf8, 1, static_cast<size_t>(cb), g_log);
    }
  }
  va_end(args);
  fflush(g_log);
}

// ---- [0] 对齐官方 install_ime_file（imesetup.cpp:141-168）----
// 复制 rime_tsf.dll 到 System32\beautype_tsf.dll，然后 ShellExecuteExW 调用 regsvr32
// 前向声明（RunRegsvr32 定义在 InstallDll 之后，C3861 前向引用）
static bool RunRegsvr32(const std::wstring& regsvr32_path, const std::wstring& dll_path);
static bool RunRegsvr32Unreg(const std::wstring& regsvr32_path, const std::wstring& dll_path);

// ---- v4.7.6：完全照官方 copy_file/delete_file（imesetup.cpp:32-59）----
// 系统目录 DLL 常被已加载它的进程占用（TSF 输入法 DLL 会被 TextInputHost 及
// 使用过输入法的应用进程加载，仅共享读/删除、不共享写）。裸 CopyFileW/DeleteFileW
// 在该情况下直接失败（实机 2026-10-12：64 位 beautype_tsf.dll 被占用 → InstallDll
// 步骤 4 复制失败 → System32 残留旧 DLL、64 位视图 CLSID 缺失，TSF 64 位路径失效）。
// 官方策略：复制失败后尝试 MoveFileEx 把占用文件改名 dest.old.N（已加载 DLL 允许
// 改名，实测通过），并标记 MOVEFILE_DELAY_UNTIL_REBOOT 待重启删除，再重试复制。
static BOOL copy_file(const std::wstring& src, const std::wstring& dest) {
  BOOL ret = CopyFileW(src.c_str(), dest.c_str(), FALSE);
  if (!ret) {
    for (int i = 0; i < 10; ++i) {
      const std::wstring old = dest + L".old." + std::to_wstring(i);
      if (MoveFileExW(dest.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        break;
      }
    }
    ret = CopyFileW(src.c_str(), dest.c_str(), FALSE);
  }
  return ret;
}

static BOOL delete_file(const std::wstring& file) {
  BOOL ret = DeleteFileW(file.c_str());
  if (!ret) {
    for (int i = 0; i < 10; ++i) {
      const std::wstring old = file + L".old." + std::to_wstring(i);
      if (MoveFileExW(file.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        return TRUE;
      }
    }
  }
  return ret;
}
static bool InstallDll() {
  Log(L"[0] ===== InstallDll 开始 =====\n");
  // 完全照官方 imesetup.cpp:141-246（32 位进程视角，官方 WeaselSetup 仅 Win32 构建）：
  //   1) GetSystemDirectoryW 返回 "C:\Windows\System32" 字符串，但 x86 进程访问
  //      该路径被 WOW64 文件系统重定向到 SysWOW64 —— 此处先装/注册 32 位 DLL。
  //   2) is_wow64()（x86 进程在 64 位系统 = TRUE）→ Wow64DisableWow64FsRedirection
  //      → 同一路径指向真实 System32 → 装/注册 64 位 DLL → revert（官方 177-181/233-243）。
  // 源文件布局：exe 同目录 rime_tsf.dll = 64 位版；x86\rime_tsf.dll = 32 位版。

  // 1. 获取 register_tip.exe 路径（照官方 imesetup.cpp:147-156）
  Log(L"[0] 步骤1: 获取 register_tip.exe 路径\n");
  WCHAR exePath[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  Log(L"[0] exePath = %ls\n", exePath);
  WCHAR drive[_MAX_DRIVE], dir[_MAX_DIR];
  _wsplitpath_s(exePath, drive, _countof(drive), dir, _countof(dir), nullptr, 0, nullptr, 0);
  const std::wstring baseDir = std::wstring(drive) + dir;

  // 2. GetSystemDirectoryW（照官方 imesetup.cpp:158-159）
  Log(L"[0] 步骤2: 获取 System32 路径\n");
  WCHAR sysDir[MAX_PATH] = {};
  GetSystemDirectoryW(sysDir, _countof(sysDir));
  const std::wstring destDll = std::wstring(sysDir) + L"\\beautype_tsf.dll";
  Log(L"[0] destDll = %ls\n", destDll.c_str());

  // 3. 复制 32 位 DLL 并注册（x86 进程访问 destDll 被 WOW64 重定向到 SysWOW64）
  const std::wstring srcDll32 = baseDir + L"x86\\beautype_tsf.dll";
  Log(L"[0] 步骤3: 复制 32 位 DLL: %ls -> %ls\n", srcDll32.c_str(), destDll.c_str());
  if (!copy_file(srcDll32, destDll)) {
    // v4.7.10：失败诊断增强——copy_file 内部已含官方"占用改名重试"（MoveFileEx 改名
    // dest.old.N + DELAY_UNTIL_REBOOT 后重试复制，imesetup.cpp:32-59），仍失败说明
    // 非占用问题。err=5(ACCESS_DENIED) 在提权安装下多为杀毒软件实时防护拦截系统目录
    // 写入（无签名 DLL + 安装器子进程写 System32 = AV 重点行为；对方真机 2026-10-15
    // 实测 err=5，本机同包成功 → 环境拦截）。日志给出可操作处理指引，避免用户盲试。
    const DWORD err = GetLastError();
    Log(L"[0] copy_file(32 位) 失败: err=%lu (0x%08lX)\n", err, err);
    Log(L"[0] 诊断: 目标 %ls 已存在=%d；err=5(拒绝访问) 在管理员安装下多为杀毒软件/"
        L"安全软件实时防护拦截系统目录写入，请关闭实时防护或添加白名单后重试，"
        L"或重启电脑后再装（旧 DLL 占用）；err=32 为文件被占用（已自动改名重试过）。\n",
        destDll.c_str(), ::GetFileAttributesW(destDll.c_str()) != INVALID_FILE_ATTRIBUTES);
    return false;
  }
  if (!RunRegsvr32(std::wstring(sysDir) + L"\\regsvr32.exe", destDll)) return false;

  // 4. 64 位 DLL：is_wow64() → disable FS redirection → 真实 System32（官方 169-243）
  BOOL isWow64 = FALSE;
  if (IsWow64Process(GetCurrentProcess(), &isWow64) && isWow64) {
    PVOID OldValue = nullptr;
    if (!Wow64DisableWow64FsRedirection(&OldValue)) {
      Log(L"[0] Wow64DisableWow64FsRedirection 失败: err=%lu\n", GetLastError());
      return false;
    }
    const std::wstring srcDll64 = baseDir + L"beautype_tsf.dll";
    Log(L"[0] 步骤4: 复制 64 位 DLL: %ls -> %ls\n", srcDll64.c_str(), destDll.c_str());
    if (!copy_file(srcDll64, destDll)) {
      Log(L"[0] copy_file(64 位) 失败: err=%lu\n", GetLastError());
      Wow64RevertWow64FsRedirection(OldValue);
      return false;
    }
    // System32\regsvr32.exe（disable 后同一 sysDir 字符串指向真实 System32）
    const BOOL ok = RunRegsvr32(std::wstring(sysDir) + L"\\regsvr32.exe", destDll);
    Wow64RevertWow64FsRedirection(OldValue);
    if (!ok) return false;
  } else {
    Log(L"[0] 非 WOW64 系统：仅注册 32 位 DLL\n");
  }

  return true;
}

// 官方 imesetup.cpp:353-365：ShellExecuteExW 调用 regsvr32 /s 并等待结束
static bool RunRegsvr32(const std::wstring& regsvr32_path, const std::wstring& dll_path) {
  std::wstring params = L"/s \"" + dll_path + L"\"";
  Log(L"[0] ShellExecuteExW: %ls %ls\n", regsvr32_path.c_str(), params.c_str());
  for (int attempt = 0; attempt < 2; ++attempt) {  // 2026-10-14：失败重试一次（首装曾静默失败致 CLSID 缺失）
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = regsvr32_path.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOW;
    Log(L"[0] 调用 ShellExecuteExW (尝试 %d)...\n", attempt + 1);
    if (!ShellExecuteExW(&sei)) {
      Log(L"[0] ShellExecuteExW 失败: err=%lu\n", GetLastError());
      return false;
    }
    Log(L"[0] WaitForSingleObject 开始（等待 regsvr32 结束）...\n");
    DWORD waitResult = WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exitCode = 0;
    if (waitResult == WAIT_OBJECT_0)
      GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);
    Log(L"[0] regsvr32 结束: wait=%lu exitCode=%lu %ls\n", waitResult, exitCode,
        exitCode == 0 ? L"(成功)" : L"(失败!)");
    if (exitCode == 0) return true;
    // regsvr32 失败（位数不匹配弹窗被跳过 / DllRegisterServer 返回错误）→ 重试
    Log(L"[0] regsvr32 失败将重试: %ls\n", dll_path.c_str());
  }
  return false;
}

// ---- [0u] 对齐官方 uninstall_ime_file（imesetup.cpp:248-293，32 位进程视角）----
// 卸载顺序与官方反向安装一致：先 SysWOW64 反注册/删除 32 位 DLL，
// 再 disable FS redirection 反注册/删除真实 System32 的 64 位 DLL。
static bool UninstallDll() {
  Log(L"[0u] ===== UninstallDll 开始 =====\n");
  WCHAR sysDir[MAX_PATH] = {};
  GetSystemDirectoryW(sysDir, _countof(sysDir));
  const std::wstring dllPath = std::wstring(sysDir) + L"\\beautype_tsf.dll";

  // 1. 反注册 + 删除 32 位 DLL（x86 进程访问 dllPath 被 WOW64 重定向到 SysWOW64）
  Log(L"[0u] 32 位 DLL 反注册/删除: %ls\n", dllPath.c_str());
  if (!RunRegsvr32Unreg(std::wstring(sysDir) + L"\\regsvr32.exe", dllPath)) return false;
  if (!delete_file(dllPath)) {
    Log(L"[0u] delete_file(32 位) 失败: err=%lu\n", GetLastError());
  }

  // 2. 64 位 DLL：is_wow64() → disable FS redirection → 真实 System32（官方 259-291）
  BOOL isWow64 = FALSE;
  if (IsWow64Process(GetCurrentProcess(), &isWow64) && isWow64) {
    PVOID OldValue = nullptr;
    if (!Wow64DisableWow64FsRedirection(&OldValue)) {
      Log(L"[0u] Wow64DisableWow64FsRedirection 失败: err=%lu\n", GetLastError());
      return false;
    }
    const BOOL ok = RunRegsvr32Unreg(std::wstring(sysDir) + L"\\regsvr32.exe", dllPath);
    Log(L"[0u] 删除 64 位 DLL: %ls\n", dllPath.c_str());
    delete_file(dllPath);
    Wow64RevertWow64FsRedirection(OldValue);
    if (!ok) return false;
  } else {
    Log(L"[0u] 非 WOW64 系统：仅反注册 32 位 DLL\n");
  }

  return true;
}

// 官方 imesetup.cpp:334-365：ShellExecuteExW 调用 regsvr32 /u /s 并等待结束
static bool RunRegsvr32Unreg(const std::wstring& regsvr32_path, const std::wstring& dll_path) {
  std::wstring params = L"/u /s \"" + dll_path + L"\"";
  Log(L"[0u] ShellExecuteExW: %ls %ls\n", regsvr32_path.c_str(), params.c_str());
  SHELLEXECUTEINFOW sei = {};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"open";
  sei.lpFile = regsvr32_path.c_str();
  sei.lpParameters = params.c_str();
  sei.nShow = SW_SHOW;
  if (!ShellExecuteExW(&sei)) {
    Log(L"[0u] ShellExecuteExW 失败: err=%lu\n", GetLastError());
    return false;
  }
  DWORD waitResult = WaitForSingleObject(sei.hProcess, INFINITE);
  DWORD exitCode = 0;
  if (waitResult == WAIT_OBJECT_0)
    GetExitCodeProcess(sei.hProcess, &exitCode);
  CloseHandle(sei.hProcess);
  Log(L"[0u] regsvr32 反注册结束: wait=%lu exitCode=%lu %ls\n", waitResult,
      exitCode, exitCode == 0 ? L"(成功)" : L"(失败!)");
  return exitCode == 0;
}

// ---- 注册表辅助（[U1]/[U3] 反注册用；CLSID 注册/反注册本体已回归 regsvr32）----
static void DeleteRegTree(HKEY root, const wchar_t* sub) {
  if (RegDeleteTreeW(root, sub) != ERROR_SUCCESS &&
      GetLastError() != ERROR_FILE_NOT_FOUND)
    Log(L"[U] RegDeleteTreeW 失败: %ls (err=%lu)\n", sub, GetLastError());
}

// ---- [U4] v4.7.6 清理 HKCU 输入法列表死项（用户级 CTF SortOrder） ----
// 现象（实机 2026-10-12）：卸载后 Windows 输入法列表
// （HKCU\Software\Microsoft\CTF\SortOrder\AssemblyItem）仍残留 Beautype 项。
// 系统级注册（CLSID/CTF/Run）已删，但用户按 Win+Space / 语言栏切换输入法时
// 系统仍按该列表尝试激活 Beautype（其 CLSID 已不存在）→ 切不出输入法、
// 打不了中文（微软拼音从头到尾正常，非系统故障；用户实测手动删除该项后
// 立即恢复）。该项是 Windows 在用户切换输入法时自动写入的用户级数据；
// 官方 weasel 卸载同样不清（官方 DLL 卸载后死项同样导致切换异常，我们已
// 实测中招）。
// 方案（用户确认）：枚举 HKCU\Software\Microsoft\CTF\SortOrder\AssemblyItem
// 下所有子键（含各 LCID 分支如 0x00000804、容器 GUID 如 {34745C63-...}、
// 0000000N 序号项），对每个序号项读取 CLSID 值，等于 Beautype CLSID 则删除
// 该子键（RegDeleteKeyW，HKCU 当前用户，无需提权）。删除后若容器/LCID 因
// 本次删除变空则顺带清理空父链（仅清变空的键，不动微软拼音等其他输入法的
// 项——同一容器 {34745C63-...} 下 00000000 是微软拼音，必须保留）。
// 多用户系统只清当前用户 HKCU（对齐官方不扩大范围）。
static void CleanupHkcuSortOrderDeadItem() {
  const wchar_t* kAssemblyItem = L"Software\\Microsoft\\CTF\\SortOrder\\AssemblyItem";
  wchar_t clsid_str[64];
  StringFromGUID2(kClsidBeautype, clsid_str, 64);
  Log(L"[U4] 清理 HKCU SortOrder 死项（目标 CLSID=%ls）\n", clsid_str);

  HKEY hAssembly = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kAssemblyItem, 0, KEY_READ, &hAssembly) !=
      ERROR_SUCCESS) {
    Log(L"[U4] HKCU SortOrder\\AssemblyItem 不存在（无残留）\n");
    return;
  }
  bool cleaned_any = false;
  for (DWORD lcid_idx = 0;; ++lcid_idx) {
    wchar_t lcid_name[64];
    DWORD lcid_len = _countof(lcid_name);
    FILETIME ft{};
    if (RegEnumKeyExW(hAssembly, lcid_idx, lcid_name, &lcid_len, nullptr,
                      nullptr, nullptr, &ft) != ERROR_SUCCESS)
      break;  // 枚举完所有 LCID 分支（如 0x00000804 简体中文；0x00000409 英文键盘）
    HKEY hLcid = nullptr;
    if (RegOpenKeyExW(hAssembly, lcid_name, 0, KEY_READ | KEY_ENUMERATE_SUB_KEYS,
                      &hLcid) != ERROR_SUCCESS)
      continue;
    for (DWORD cont_idx = 0;; ++cont_idx) {
      wchar_t cont_name[64];
      DWORD cont_len = _countof(cont_name);
      if (RegEnumKeyExW(hLcid, cont_idx, cont_name, &cont_len, nullptr, nullptr,
                        nullptr, &ft) != ERROR_SUCCESS)
        break;  // 枚举完该 LCID 下所有容器 GUID
      HKEY hCont = nullptr;
      if (RegOpenKeyExW(hLcid, cont_name, 0, KEY_READ | KEY_ENUMERATE_SUB_KEYS,
                        &hCont) != ERROR_SUCCESS)
        continue;
      for (DWORD item_idx = 0;; ++item_idx) {
        wchar_t item_name[64];
        DWORD item_len = _countof(item_name);
        if (RegEnumKeyExW(hCont, item_idx, item_name, &item_len, nullptr, nullptr,
                          nullptr, &ft) != ERROR_SUCCESS)
          break;  // 枚举完该容器下所有序号项（0000000N）
        // 序号项读 CLSID 值（注册表值名不区分大小写），命中 Beautype 才删
        wchar_t clsid_val[64] = {};
        DWORD cb = sizeof(clsid_val);
        DWORD type = 0;
        const LSTATUS r = RegGetValueW(hCont, item_name, L"CLSID", RRF_RT_REG_SZ,
                                       &type, clsid_val, &cb);
        if (r != ERROR_SUCCESS || type != REG_SZ || clsid_val[0] == L'\0')
          continue;
        if (_wcsicmp(clsid_val, clsid_str) != 0)
          continue;  // 微软拼音等其他输入法，不得误删
        const std::wstring item_path = std::wstring(lcid_name) + L"\\" +
                                       cont_name + L"\\" + item_name;
        const LSTATUS rd = RegDeleteKeyW(hCont, item_name);
        Log(L"[U4] 删除 HKCU SortOrder 死项: %ls (CLSID=%ls) result=%d\n",
            item_path.c_str(), clsid_val, static_cast<int>(rd));
        cleaned_any = true;
      }
      // （可选）本次删除导致容器变空 → 清理空容器键
      DWORD sub = 0;
      if (RegQueryInfoKeyW(hCont, nullptr, nullptr, nullptr, &sub, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr) == ERROR_SUCCESS &&
          sub == 0) {
        Log(L"[U4] 清理空容器键: %ls\\%ls\n", lcid_name, cont_name);
        RegDeleteKeyW(hLcid, cont_name);
      }
      RegCloseKey(hCont);
    }
    // 该 LCID 分支因本次清理变空 → 删除（如 0x00000409 英文键盘等非目标分支
    // 不会因 Beautype 清理而变空，不会误删）
    DWORD sub2 = 0;
    if (RegQueryInfoKeyW(hLcid, nullptr, nullptr, nullptr, &sub2, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr) == ERROR_SUCCESS &&
        sub2 == 0) {
      Log(L"[U4] 清理空 LCID 分支: %ls\n", lcid_name);
      RegDeleteKeyW(hAssembly, lcid_name);
    }
    RegCloseKey(hLcid);
  }
  RegCloseKey(hAssembly);
  if (!cleaned_any)
    Log(L"[U4] HKCU SortOrder 无 Beautype 死项（无残留或系统已重写）\n");
}

// ---- v4.7.9：注册自检（安装完成后回读注册表双视图，防 §26 类"regsvr32 写错视图"
// 静默回归与"复制失败但流程继续"的假成功）----
// 健康基准（本机实测 2026-10-15）：CLSID InprocServer32 与 CTF TIP 在 64/32 两视图
// 均存在。CLSID 双视图为硬性要求（regsvr32 双视图注册的决定性证据）；TIP 至少
// 32 位视图（x86 进程 RegisterProfile 的写入视图），64 位视图存在性记录不判失败。
static bool VerifyRegistered() {
  wchar_t clsid_str[64];
  StringFromGUID2(kClsidBeautype, clsid_str, 64);
  const std::wstring clsid_key =
      L"CLSID\\" + std::wstring(clsid_str) + L"\\InprocServer32";
  const std::wstring tip_key =
      L"SOFTWARE\\Microsoft\\CTF\\TIP\\" + std::wstring(clsid_str);

  bool clsid64 = false, clsid32 = false, tip64 = false, tip32 = false;
  auto probe = [&](REGSAM view, bool& clsidOut, bool& tipOut) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", 0,
                      KEY_READ | view, &hk) == ERROR_SUCCESS) {
      HKEY hsub = nullptr;
      if (RegOpenKeyExW(hk, clsid_key.c_str(), 0, KEY_READ, &hsub) ==
          ERROR_SUCCESS) {
        clsidOut = true;
        RegCloseKey(hsub);
      }
      RegCloseKey(hk);
    }
    HKEY ht = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, tip_key.c_str(), 0, KEY_READ | view,
                      &ht) == ERROR_SUCCESS) {
      tipOut = true;
      RegCloseKey(ht);
    }
  };
  probe(KEY_WOW64_64KEY, clsid64, tip64);
  probe(KEY_WOW64_32KEY, clsid32, tip32);
  Log(L"[S] 注册自检: CLSID 64视图=%d 32视图=%d, TIP 64视图=%d 32视图=%d\n",
      clsid64 ? 1 : 0, clsid32 ? 1 : 0, tip64 ? 1 : 0, tip32 ? 1 : 0);
  return clsid64 && clsid32 && tip32;
}

int wmain(int argc, wchar_t* argv[]) {
  // 对齐官方 WeaselSetup 参数体系（WeaselSetup.cpp:253-268）：
  //   /i = 安装，/s = 简体安装，/t = 繁体安装，/u = 卸载
  // Beautype 仅简体方案：/t 行为同 /i；无参兼容历史安装调用。
  const wchar_t* arg = argc > 1 ? argv[1] : L"";
  const bool uninstall = wcscmp(arg, L"/u") == 0 ||
                         wcscmp(arg, L"-u") == 0 ||
                         wcscmp(arg, L"/uninstall") == 0;
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring log_path(self);
  log_path = log_path.substr(0, log_path.find_last_of(L"\\")) + L"\\register_tip.log";
  // v4.7.9：二进制打开 + UTF-8 字节直写 + BOM（记事本正确显示；§16.16 禁 ccs=UTF-8）
  g_log = _wfopen(log_path.c_str(), L"wb");
  if (g_log != nullptr) {
    const unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
    fwrite(kBom, 1, sizeof(kBom), g_log);
  }
  Log(L"=== Beautype register_tip (arg=%ls, %s) ===\n", arg,
      uninstall ? L"uninstall" : L"install");
  const LANGID lang = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);

  // ---- 卸载模式：反注册 + 清理 ----
  if (uninstall) {
    // [0u] 对齐官方 uninstall_ime_file：ShellExecuteExW 调用 regsvr32 /u + 删除 DLL
    UninstallDll();

    // [U1] 删 TSF TIP 注册（CTF）——删除即等效 DisableLanguageProfile
    // （注：Win11 SDK 的 ITfInputProcessorProfiles 已无 DisableLanguageProfile，
    //  删除 HKLM\...\CTF\TIP\{CLSID} 树达到同样效果）
    {
      wchar_t clsid_str[64];
      StringFromGUID2(kClsidBeautype, clsid_str, 64);
      const std::wstring tip = L"SOFTWARE\\Microsoft\\CTF\\TIP\\" +
                               std::wstring(clsid_str);
      DeleteRegTree(HKEY_LOCAL_MACHINE, tip.c_str());
      Log(L"[U1] CTF TIP 键已清理\n");
    }
    // [U2] InstallLayoutOrTip 反安装
    {
      HMODULE hInput = LoadLibraryW(L"input.dll");
      if (hInput != nullptr) {
        PTF_INSTALLLAYOUTORTIP pfn =
            (PTF_INSTALLLAYOUTORTIP)GetProcAddress(hInput, "InstallLayoutOrTip");
        if (pfn != nullptr) {
          const wchar_t* kTitle =
              L"0804:{B17E78C8-ABAF-4891-8441-42FC45F475A4}"
              L"{FC89E162-09DE-4842-A7C7-7F9BB73E888E}";
          const BOOL ok = pfn(kTitle, ILOT_UNINSTALL);
          Log(L"[U2] InstallLayoutOrTip(UNINSTALL): ok=%d\n", ok ? 1 : 0);
        }
        FreeLibrary(hInput);
      }
    }
    // [U3] 删 COM CLSID 树（32 位 + 64 位视图显式删除；实测 HKCR 句柄在
    // WOW64 下 RegDeleteTreeW 删不掉 WOW6432Node 键，与 tsf_dll.cpp 的
    // DllUnregisterServer 同款逻辑）
    {
      wchar_t clsid_str[64];
      StringFromGUID2(kClsidBeautype, clsid_str, 64);
      const std::wstring clsid_key = L"CLSID\\" + std::wstring(clsid_str);
      HKEY hk = nullptr;
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", 0,
                        KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_32KEY, &hk) == ERROR_SUCCESS) {
        RegDeleteTreeW(hk, clsid_key.c_str());
        RegCloseKey(hk);
      }
      if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", 0,
                        KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        RegDeleteTreeW(hk, clsid_key.c_str());
        RegCloseKey(hk);
      }
      Log(L"[U3] COM CLSID 键已清理\n");
    }
    // [U4] 清理 HKCU 输入法列表死项（见 CleanupHkcuSortOrderDeadItem 注释：
    // 卸载只清系统级注册，用户切换输入法时 Windows 自动写入的 HKCU SortOrder
    // 残留项会在 CLSID 删除后导致切换异常/打不了中文，须按 CLSID 匹配删除）
    CleanupHkcuSortOrderDeadItem();
    Log(L"UNREGISTER_TIP_DONE\n");
    if (g_log != nullptr) std::fclose(g_log);
    return 0;
  }

  // ---- [0] 对齐官方 install_ime_file：复制 DLL 到 System32 + ShellExecuteExW 调用 regsvr32
  // v4.7.9：InstallDll 返回值必须传播——复制/regsvr32 任一失败 = 注册失败，立即中止并
  // 返回非 0（官方 imesetup.cpp 同环节失败 return 1；旧版忽略返回值：对方机器 32 位
  // DLL 复制失败仍打 REGISTER_TIP_DONE 恒 exit 0 → NSIS 假成功——M1 复盘 §28）。
  if (!InstallDll()) {
    Log(L"REGISTER_TIP_FAILED: InstallDll（DLL 复制/regsvr32 注册失败，err 见上方）\n");
    if (g_log != nullptr) std::fclose(g_log);
    return 1;
  }

  // ---- A) RegisterProfiles（照 weasel Register.cpp RegisterProfiles()）----
  // ITfInputProcessorProfileMgr::RegisterProfile：注册 profile 描述、图标并启用。
  HRESULT hrStepA = S_OK;
  {
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ITfInputProcessorProfileMgr* pMgr = nullptr;
    const HRESULT hr = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_ALL,
        IID_ITfInputProcessorProfileMgr, (LPVOID*)&pMgr);
    if (FAILED(hr) || pMgr == nullptr) {
      hrStepA = hr;
      Log(L"[A] ITfInputProcessorProfileMgr CoCreate FAILED: hr=0x%08X\n", hr);
    } else {
      // 图标 = 安装后的 TIP DLL（对齐官方 Register.cpp:72-80：
      // IconFile = g_hInst = 安装后 DLL 自身路径；regsvr32 注册时 DLL 已在系统目录）
      // x86 进程 GetSystemDirectoryW 返回 "C:\Windows\System32"：64 位进程解析到
      // 真实 System32 的 64 位 beautype_tsf.dll，32 位进程经 WOW64 重定向到
      // SysWOW64 的 32 位版——双视图 DLL 均含图标资源（tsf/resource.rc IDI_ICON1）。
      wchar_t sysdir[MAX_PATH];
      GetSystemDirectoryW(sysdir, ARRAYSIZE(sysdir));
      const std::wstring icon = std::wstring(sysdir) + L"\\beautype_tsf.dll";
      const std::wstring desc = L"Beautype 输入法";
      const HRESULT hrR = pMgr->RegisterProfile(
          kClsidBeautype, lang, kProfileBeautype, desc.c_str(),
          static_cast<ULONG>(desc.size() * sizeof(wchar_t)), icon.c_str(),
          static_cast<ULONG>(icon.size() * sizeof(wchar_t)), 0 /*uIconIndex*/,
          nullptr /*hkl*/, 0 /*dwFlags*/, TRUE /*fEnable*/, 0 /*dwCategory*/);
      hrStepA = hrR;
      Log(L"[A] RegisterProfile: hr=0x%08X\n", hrR);
      pMgr->Release();
    }
    if (SUCCEEDED(hrCo)) CoUninitialize();
  }
  if (FAILED(hrStepA)) {
    Log(L"REGISTER_TIP_FAILED: RegisterProfile hr=0x%08X\n", hrStepA);
    if (g_log != nullptr) std::fclose(g_log);
    return 1;
  }

  // ---- B) RegisterCategories（照 weasel Register.cpp RegisterCategories()）----
  // ITfCategoryMgr::RegisterCategory：注册全部 16 个支持类别 GUID。
  // 创建方式：官方 msctf.dll 导出 TF_CreateCategoryMgr()（不依赖 COM 类工厂）。
  {
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ITfCategoryMgr* pCat = nullptr;
    HMODULE hMsctf = LoadLibraryW(L"msctf.dll");
    PTF_CREATECATEGORYMGR pfnCreateCat =
        hMsctf ? (PTF_CREATECATEGORYMGR)GetProcAddress(hMsctf, "TF_CreateCategoryMgr")
               : nullptr;
    HRESULT hr = pfnCreateCat ? pfnCreateCat(&pCat) : E_FAIL;
    if (FAILED(hr) || pCat == nullptr) {
      Log(L"[B] TF_CreateCategoryMgr FAILED: hr=0x%08X (msctf loaded=%d)\n", hr,
          hMsctf ? 1 : 0);
    } else {
      static const GUID kSupportCategories[] = {
          GUID_TFCAT_CATEGORY_OF_TIP, GUID_TFCAT_TIP_KEYBOARD,
          GUID_TFCAT_TIPCAP_SECUREMODE, GUID_TFCAT_TIPCAP_UIELEMENTENABLED,
          GUID_TFCAT_TIPCAP_INPUTMODECOMPARTMENT, GUID_TFCAT_TIPCAP_COMLESS,
          GUID_TFCAT_TIPCAP_WOW16, GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT,
          GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT, GUID_TFCAT_PROP_AUDIODATA,
          GUID_TFCAT_PROP_INKDATA, GUID_TFCAT_PROPSTYLE_CUSTOM,
          GUID_TFCAT_PROPSTYLE_STATIC, GUID_TFCAT_PROPSTYLE_STATICCOMPACT,
          GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, GUID_TFCAT_DISPLAYATTRIBUTEPROPERTY};
      HRESULT hrCat = S_OK;
      int nCat = 0;
      for (const auto& guid : kSupportCategories) {
        const HRESULT h =
            pCat->RegisterCategory(kClsidBeautype, guid, kClsidBeautype);
        if (SUCCEEDED(h)) ++nCat;
        if (FAILED(h)) hrCat = h;
      }
      Log(L"[B] RegisterCategories: hr=0x%08X (%d/16 GUIDs)\n", hrCat, nCat);
      if (FAILED(hrCat) || nCat != 16) {
        Log(L"REGISTER_TIP_FAILED: RegisterCategories hr=0x%08X (%d/16)\n",
            hrCat, nCat);
        pCat->Release();
        if (hMsctf != nullptr) FreeLibrary(hMsctf);
        if (SUCCEEDED(hrCo)) CoUninitialize();
        if (g_log != nullptr) std::fclose(g_log);
        return 1;
      }
      pCat->Release();
    }
    if (hMsctf != nullptr) FreeLibrary(hMsctf);
    if (SUCCEEDED(hrCo)) CoUninitialize();
  }

  // ---- C) EnableLanguageProfile + ByDefault（照 weasel enable_profile(TRUE)）----
  // v4.7.9：两调用任一失败即整体失败（对方机器 ByDefault hr=E_FAIL 但旧版不检查，
  // 输入法仍不可用——M1 复盘 §28）。
  HRESULT hrStepC1 = S_OK, hrStepC2 = S_OK;
  {
    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ITfInputProcessorProfiles* pProfiles = nullptr;
    const HRESULT hr = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfiles, (LPVOID*)&pProfiles);
    if (FAILED(hr) || pProfiles == nullptr) {
      hrStepC1 = hr;
      hrStepC2 = hr;
      Log(L"[C] ITfInputProcessorProfiles CoCreate FAILED: hr=0x%08X\n", hr);
    } else {
      const HRESULT hr1 =
          pProfiles->EnableLanguageProfile(kClsidBeautype, lang, kProfileBeautype, TRUE);
      const HRESULT hr2 =
          pProfiles->EnableLanguageProfileByDefault(kClsidBeautype, lang, kProfileBeautype, TRUE);
      hrStepC1 = hr1;
      hrStepC2 = hr2;
      Log(L"[C] EnableLanguageProfile: hr=0x%08X, ByDefault: hr=0x%08X\n", hr1, hr2);
      pProfiles->Release();
    }
    if (SUCCEEDED(hrCo)) CoUninitialize();
  }
  if (FAILED(hrStepC1) || FAILED(hrStepC2)) {
    Log(L"REGISTER_TIP_FAILED: EnableLanguageProfile hr=0x%08X, ByDefault hr=0x%08X\n",
        hrStepC1, hrStepC2);
    if (g_log != nullptr) std::fclose(g_log);
    return 1;
  }

  // ---- D) InstallLayoutOrTip（照 weasel imesetup.cpp install()，最后执行）----
  // v4.7.9：ok=0 即失败并中止（旧版只记录不检查；对方机器 ok=1 正常，
  // 故 D 不是对方失败点，但按官方语义仍需传播失败）。
  {
    HMODULE hInput = LoadLibraryW(L"input.dll");
    if (hInput == nullptr) {
      Log(L"[D] input.dll load FAILED (err=%lu)\n", GetLastError());
      Log(L"REGISTER_TIP_FAILED: input.dll load FAILED\n");
      if (g_log != nullptr) std::fclose(g_log);
      return 1;
    } else {
      PTF_INSTALLLAYOUTORTIP pfn =
          (PTF_INSTALLLAYOUTORTIP)GetProcAddress(hInput, "InstallLayoutOrTip");
      if (pfn == nullptr) {
        Log(L"[D] InstallLayoutOrTip NOT FOUND in input.dll\n");
        Log(L"REGISTER_TIP_FAILED: InstallLayoutOrTip NOT FOUND\n");
        FreeLibrary(hInput);
        if (g_log != nullptr) std::fclose(g_log);
        return 1;
      } else {
        // 官方格式："%04X:{CLSID}{PROFILEGUID}"，langid=0x0804(简体中文)
        const wchar_t* kTitle =
            L"0804:{B17E78C8-ABAF-4891-8441-42FC45F475A4}"
            L"{FC89E162-09DE-4842-A7C7-7F9BB73E888E}";
        const BOOL ok = pfn(kTitle, 0);
        Log(L"[D] InstallLayoutOrTip: ok=%d (TRUE=成功)\n", ok ? 1 : 0);
        if (!ok) {
          Log(L"REGISTER_TIP_FAILED: InstallLayoutOrTip ok=0\n");
          FreeLibrary(hInput);
          if (g_log != nullptr) std::fclose(g_log);
          return 1;
        }
      }
      FreeLibrary(hInput);
    }
  }

  // v4.7.0：CLSID 注册/反注册回归 regsvr32（官方同款，DllRegisterServer/
  // DllUnregisterServer 在 tsf_dll.cpp）；本 exe 只做 RegisterProfile +
  // RegisterCategories + EnableLanguageProfile + InstallLayoutOrTip。
  // v4.7.9：注册自检（回读注册表双视图）通过后才打 DONE；任一环节失败在各自
  // 分支已 return 1 —— NSIS 据此弹窗中止，不再"安装流程正常但输入法未注册"。
  if (!VerifyRegistered()) {
    Log(L"REGISTER_TIP_FAILED: 注册自检未通过（CLSID/TIP 注册表键缺失，见 [S] 行）\n");
    if (g_log != nullptr) std::fclose(g_log);
    return 1;
  }

  Log(L"REGISTER_TIP_DONE\n");
  if (g_log != nullptr) std::fclose(g_log);
  return 0;
}
