// rime_tsf.dll —— Beautype 系统级 TSF 输入法（最终形态，M1.1）。
//
// 设计对照小狼毫（weasel）官方 TSF 组件：
//   - COM 外壳：DllGetClassObject / DllRegisterServer / DllUnregisterServer，
//     由 regsvr32 注册（weasel 官方同样经 regsvr32 注册 TSF dll）。
//   - TSF 服务注册（RegisterProfile / RegisterCategory / EnableLanguageProfile /
//     InstallLayoutOrTip）由 register_tip.exe 经官方 API 完成（照 weasel
//     imesetup.cpp + WeaselTSF/Register.cpp），本 DLL 不直接写 TSF 注册表
//     （微软官方明确：不建议直接写注册表设置 IME）。
//   - 服务化（2026-10-06）：本 DLL 不加载 librime 引擎（服务进程 rime_server.exe
//     承载引擎/候选窗/托盘），经官方 WeaselIPC Client 转发按键并显示组合。
#include <windows.h>
#include <msctf.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <string>

#include "text_service.h"
#include "engine/rime_engine.h"
#include "log/log.h"

// 统一日志文件句柄由 rime_log 库定义（log/log.cpp）；v4.7.6 起本 DLL 在
// DllGetClassObject（COM 首次实例化，loader lock 之外）打开
// %APPDATA%\Beautype\rime_tsf.log 并赋值（与服务进程 rime_server.log 独立）。

// ---- 身份常量（CLSID/Profile）定义移至 tsf/tsf_guid.cpp（rime_frontend 共用，
// test_tsf 链接时也能解析）；本文件仅保留声明供 DllGetClassObject/注册使用 ----
extern "C" const GUID CLSID_BeautypeTSF;

static const wchar_t kDisplayName[] = L"Beautype 输入法";

// regsvr32/宿主进程调用本 DLL 导出函数时，GetModuleFileNameW(nullptr) 返回的是
// 调用方（regsvr32/宿主 exe）的路径；必须用 DllMain 保存的本 DLL 模块句柄。
// v4.7.5：g_hinst 定义移到 text_service.cpp（rime_frontend 静态库内含本 DLL 的
// 两个消费方 text_service/language_bar），此处仅 extern 声明——部署改名
// beautype_tsf.dll 后 GetModuleHandleW(L"rime_tsf.dll") 返回空（真机根因：
// 语言栏图标蓝底白问号、DllDirectory 取不到目录），一律经 g_hinst 取本 DLL
// 句柄（对齐官方 WeaselTSF/Globals.cpp 的 g_hInst + Globals.h extern）。
extern HMODULE g_hinst;

namespace {

// 宽字符 → UTF-8（引擎接口用）
std::string ToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
  std::string s(n > 1 ? static_cast<std::size_t>(n - 1) : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  }
  return s;
}

// v4.7.6：恢复 DLL 侧日志（观测闭环，纯诊断不改行为）。官方 WeaselTSF 静默
// （no_logging.h，官方 dllmain.cpp:60-73 极简不做文件 I/O），无官方对应；
// log/log.cpp 设计注释即指定本 DLL 打开 %APPDATA%\Beautype\rime_tsf.log
// （与服务进程 rime_server.log 独立）。不放 DllMain：loader lock 内禁止
// 文件 I/O（v4.7.5 已对齐官方移除原 OpenLogFile）；放 DllGetClassObject
// （COM 首次实例化入口，loader lock 之外，TSF 宿主加载本 DLL 激活即触发）。
// 每进程只开一次（静态标志），追加写不截断。
void OpenDllLog() {
  static bool opened = false;
  if (opened || ::ime::g_log != nullptr) {
    return;
  }
  opened = true;
  const wchar_t* appdata = _wgetenv(L"APPDATA");
  if (appdata == nullptr || *appdata == L'\0') {
    return;
  }
  std::filesystem::path dir(appdata);
  dir /= L"Beautype";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const auto path = dir / L"rime_tsf.log";
  // 日志轮转：当前文件 >10MB → 删旧 .1 → 改名 .1（上限≈20MB/份，同 server）
  {
    const auto path1 = dir / L"rime_tsf.log.1";
    if (std::filesystem::file_size(path, ec) > 10LL * 1024 * 1024) {
      std::filesystem::remove(path1, ec);
      std::filesystem::rename(path, path1, ec);
    }
  }
  ::ime::g_log = std::fopen(ToUtf8(path.wstring()).c_str(), "a");
  if (::ime::g_log != nullptr) {
    // 日志级别：默认随构建（测试 0 / 发布 1）；config\log_level 覆盖（排障开 0）
    const auto lv_path = dir / L"config" / L"log_level";
    std::ifstream lf(lv_path);
    int lv = -1;
    if (lf >> lv) ::ime::SetLogLevel(lv);
    ::ime::LogF("[tsf] rime_tsf.log opened pid=%lu log_level=%d（%s）\n",
                ::GetCurrentProcessId(), ::ime::GetLogLevel(),
                lf ? "config 覆盖" : "默认随构建");
  }
}

// 数据目录查找（同 main.cpp FindSharedDataDir 策略）：
// DLL 所在目录逐级向上找 data/rime（自包含部署）；user = %APPDATA%\Beautype\user
bool FindDirs(std::wstring* shared_out, std::wstring* user_out) {
  wchar_t buf[MAX_PATH];
  // 必须用本 DLL 的模块句柄：宿主进程（ctfmon/应用）调用本 DLL 时，
  // GetModuleFileNameW(nullptr) 返回的是宿主 exe 的路径，会找不到 data/rime。
  if (GetModuleFileNameW(g_hinst, buf, MAX_PATH) == 0) return false;
  std::filesystem::path dir = std::filesystem::path(buf).parent_path();
  for (int i = 0; i < 8 && !dir.empty(); ++i) {
    const auto candidate = dir / L"data" / L"rime";
    if (std::filesystem::exists(candidate / L"default.yaml")) {
      *shared_out = candidate.wstring();
      break;
    }
    dir = dir.parent_path();
  }
  if (shared_out->empty()) return false;
  const wchar_t* appdata = _wgetenv(L"APPDATA");
  if (appdata == nullptr) return false;
  *user_out = (std::filesystem::path(appdata) / L"Beautype" / L"user").wstring();
  return true;
}

// 服务化（2026-10-06）：DLL 不再加载本地 librime 引擎——引擎/候选窗/托盘
// 全部在服务进程 rime_server.exe（官方 WeaselServer 架构），本 DLL 经官方
// Client（IPC 管道）转发按键并显示组合。按键由 EnsureSession 首次连接时
// 自动拉起服务进程（官方 Client::Connect 内含 ServerLauncher）。

// ---- COM 类工厂 ----
class CClassFactory final : public IClassFactory {
 public:
  CClassFactory() : ref_(1) {}

  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
      *ppv = static_cast<IClassFactory*>(this);
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

  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (outer != nullptr) return CLASS_E_NOAGGREGATION;
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    ime::RimeTextService* svc = new (std::nothrow) ime::RimeTextService();
    if (svc == nullptr) return E_OUTOFMEMORY;
    const HRESULT hr = svc->QueryInterface(riid, ppv);
    svc->Release();
    return hr;
  }

  STDMETHODIMP LockServer(BOOL) override { return S_OK; }

 private:
  volatile ULONG ref_;
};

// 注册表写入助手
HRESULT WriteSz(HKEY root, const std::wstring& path, const wchar_t* value,
                const wchar_t* value_name = nullptr) {
  HKEY key = nullptr;
  DWORD disp = 0;
  const LONG r = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                                 nullptr, &key, &disp);
  if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
  const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
  const LONG r2 = RegSetValueExW(key, value_name, 0, REG_SZ,
                                 reinterpret_cast<const BYTE*>(value), bytes);
  RegCloseKey(key);
  return r2 == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(r2);
}

HRESULT DeleteKeyTree(HKEY root, const std::wstring& path) {
  const LONG r = RegDeleteTreeW(root, path.c_str());
  return (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND)
             ? S_OK
             : HRESULT_FROM_WIN32(r);
}

}  // namespace

extern "C" {

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
  // v4.7.6：DLL 首次被 COM 实例化的入口——在这里打开 rime_tsf.log
  // （loader lock 之外；DllMain 保持官方极简，见 OpenDllLog 注释）
  OpenDllLog();
  if (!IsEqualCLSID(rclsid, CLSID_BeautypeTSF)) return CLASS_E_CLASSNOTAVAILABLE;
  if (ppv == nullptr) return E_POINTER;
  CClassFactory* factory = new (std::nothrow) CClassFactory();
  if (factory == nullptr) return E_OUTOFMEMORY;
  const HRESULT hr = factory->QueryInterface(riid, ppv);
  factory->Release();
  return hr;
}

STDAPI DllCanUnloadNow() { return S_FALSE; }

STDAPI DllRegisterServer() {
  wchar_t dll_path[MAX_PATH];
  if (GetModuleFileNameW(g_hinst, dll_path, MAX_PATH) == 0) return E_FAIL;

  wchar_t clsid_str[64];
  StringFromGUID2(CLSID_BeautypeTSF, clsid_str, 64);
  std::wstring clsid_key = L"CLSID\\";
  clsid_key += clsid_str;

  // 1) COM 注册（TSF 经 CoCreateInstance 加载本 DLL 必需）
  HRESULT hr = WriteSz(HKEY_CLASSES_ROOT, clsid_key, kDisplayName);
  if (FAILED(hr)) return hr;
  std::wstring inproc = clsid_key + L"\\InprocServer32";
  hr = WriteSz(HKEY_CLASSES_ROOT, inproc, dll_path);
  if (FAILED(hr)) return hr;
  hr = WriteSz(HKEY_CLASSES_ROOT, inproc, L"Apartment", L"ThreadingModel");
  return hr;
}

STDAPI DllUnregisterServer() {
  wchar_t clsid_str[64];
  StringFromGUID2(CLSID_BeautypeTSF, clsid_str, 64);
  const std::wstring clsid_key = L"CLSID\\" + std::wstring(clsid_str);

  // 对齐官方 imesetup 的卸载语义：CLSID 键须从 32 位与 64 位两个视图都删除。
  // 实测：RegDeleteTreeW(HKCR, ...) 在 32 位进程下删不掉 WOW6432Node 键
  //（HKCR 合并视图句柄在 WOW64 下不可靠），故显式以 KEY_WOW64_32KEY /
  // KEY_WOW64_64KEY 打开 HKLM\SOFTWARE\Classes 后分别删除。
  LONG r = ERROR_SUCCESS;
  HKEY hk = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", 0,
                    KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_32KEY, &hk) == ERROR_SUCCESS) {
    r = RegDeleteTreeW(hk, clsid_key.c_str());
    RegCloseKey(hk);
  }
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes", 0,
                    KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
    const LONG r2 = RegDeleteTreeW(hk, clsid_key.c_str());
    if (r == ERROR_SUCCESS) r = r2;
    RegCloseKey(hk);
  }
  return (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND)
             ? S_OK
             : HRESULT_FROM_WIN32(r);
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_hinst = hinst;
    // 对齐官方 WeaselTSF/dllmain.cpp：DllMain 极简，不做文件 I/O。
    // v4.7.6：DLL 侧日志（rime_tsf.log）恢复在 DllGetClassObject 打开
    // （OpenDllLog：loader lock 之外安全；原 DllMain 内 OpenLogFile 已移除）。
  }
  return TRUE;
}
