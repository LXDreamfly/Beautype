// Beautype 自动更新实现（v1.0）
//  - 版本源：GitHub Releases API（/releases/latest，只跟正式主线，跳过 prerelease）
//  - 更新源可配置：环境变量 BEAUTYPE_UPDATE_URL > %APPDATA%\Beautype\config\update.yaml
//    > 内置默认 GitHub。测试时可把更新源指向本地模拟服务器（等价 GitHub API 语义）。
//  - 交互（用户确认）：发现新版 → 弹窗"是否下载安装" → 下载 → "是否立即安装" → 运行安装器
//  - 校验：下载完成后校验文件存在且非空；若 Release body 含 "SHA256: <hash>" 行则强校验。
#include "updater.h"

#include <winhttp.h>
#include <urlmon.h>
#include <wincrypt.h>   // CryptAcquireContext/CryptHashData（SHA256）
#include <shellapi.h>   // ShellExecuteW（运行安装器）
#include <cwctype>      // towlower
#include <cstdio>
#include <string>
#include <vector>

#include "log/log.h"    // ime::g_log / ime::LogF（落到 BeautypeServer.log / rime_tsf.log）

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "urlmon.lib")

#ifndef BEAUTYPE_VERSION
#define BEAUTYPE_VERSION "0.0.0"
#endif

namespace ime::updater {
namespace {

const wchar_t kDefaultApiUrl[] =
    L"https://api.github.com/repos/LXDreamfly/Beautype/releases/latest";
const wchar_t kTempInstaller[] = L"BeautypeSetup_update.exe";
const wchar_t kUpdateCfgRel[] = L"config\\update.yaml";
const wchar_t kEnvUpdateUrl[] = L"BEAUTYPE_UPDATE_URL";

// 内置 GitHub 下载加速镜像前缀（国内网络直连 github.com 下载常被重置，
// 下载失败时按顺序尝试这些镜像；update.yaml 可用 "mirror-prefix: <url>" 追加/覆盖）。
const wchar_t* kDefaultMirrorPrefixes[] = {
    L"https://gh-proxy.com/",
    L"https://ghproxy.com/",
    L"https://mirror.ghproxy.com/",
    L"https://ghfast.top/",
    L"https://gh.llkk.cc/",
};
const int kDefaultMirrorCount =
    static_cast<int>(sizeof(kDefaultMirrorPrefixes) / sizeof(kDefaultMirrorPrefixes[0]));
const char kMirrorPrefixKey[] = "mirror-prefix:";

struct ReleaseInfo {
  std::wstring tag;     // 如 "v1.0.0"
  std::wstring exeUrl;  // 安装包浏览器下载 URL
  std::wstring sha256;  // Release body 中 "SHA256: <hex>"（无则空）
  bool valid = false;
};

// ---- 小工具：wstring ⇄ string（UTF-8） ----
std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 0 ? n - 1 : 0, '\0');
  if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  return s;
}
std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

// ---- 语义化版本比较：返回 1/0/-1（a>b / a==b / a<b） ----
// 支持 "v1.0.0" / "1.0" / "0.9.0"；只比较 major.minor.patch 三段。
int CompareVersion(const std::wstring& a, const std::wstring& b) {
  auto parts = [](const std::wstring& v) {
    std::vector<int> out;
    std::wstring cur;
    size_t start = 0;
    if (!v.empty() && (v[0] == L'v' || v[0] == L'V')) start = 1;
    for (size_t i = start; i <= v.size(); ++i) {
      if (i == v.size() || v[i] == L'.') {
        out.push_back(_wtoi(cur.c_str()));
        cur.clear();
      } else {
        cur.push_back(v[i]);
      }
    }
    while (out.size() < 3) out.push_back(0);
    return out;
  };
  const auto pa = parts(a);
  const auto pb = parts(b);
  for (int i = 0; i < 3; ++i) {
    if (pa[i] > pb[i]) return 1;
    if (pa[i] < pb[i]) return -1;
  }
  return 0;
}

// ---- 从 Release JSON 中提取字段（简单字符串解析；GitHub API 格式固定） ----
std::wstring ExtractJsonStr(const std::wstring& json, const char* key) {
  // 找 "key":"..."（ASCII key，值可为 Unicode 转义/普通字符）
  const std::string needle = std::string("\"") + key + "\":\"";
  const std::string js = WideToUtf8(json);
  size_t pos = js.find(needle);
  if (pos == std::string::npos) return std::wstring();
  pos += needle.size();
  size_t end = js.find('"', pos);
  if (end == std::string::npos) return std::wstring();
  std::string val = js.substr(pos, end - pos);
  // 简单处理 \uXXXX（GitHub tag/URL 一般无中文，URL 可能带 \u0026 之类的转义）
  size_t esc;
  while ((esc = val.find("\\u")) != std::string::npos && esc + 6 <= val.size()) {
    unsigned int code = 0;
    if (sscanf(val.c_str() + esc + 2, "%04x", &code) == 1) {
      std::wstring wc(1, static_cast<wchar_t>(code));
      std::string u8 = WideToUtf8(wc);
      val.replace(esc, 6, u8);
    } else {
      break;
    }
  }
  return Utf8ToWide(val);
}

// 找 Release body 里的 "SHA256: <hex>" 行（发布者约定格式，用于下载强校验）
std::wstring ExtractSha256(const std::wstring& json) {
  const std::string js = WideToUtf8(json);
  const std::string needle = "SHA256:";
  size_t pos = js.find(needle);
  if (pos == std::string::npos) return std::wstring();
  pos += needle.size();
  while (pos < js.size() && (js[pos] == ' ' || js[pos] == '\t')) ++pos;
  std::string hex;
  while (pos < js.size() &&
         ((js[pos] >= '0' && js[pos] <= '9') || (js[pos] >= 'a' && js[pos] <= 'f') ||
          (js[pos] >= 'A' && js[pos] <= 'F'))) {
    hex.push_back(js[pos]);
    ++pos;
  }
  return Utf8ToWide(hex);
}

// 找安装包下载 URL（assets 里 browser_download_url 且以 .exe 结尾；避开 x86 等）
std::wstring FindExeDownloadUrl(const std::wstring& json) {
  const std::string js = WideToUtf8(json);
  const std::string key = "\"browser_download_url\":\"";
  size_t pos = 0;
  while ((pos = js.find(key, pos)) != std::string::npos) {
    pos += key.size();
    size_t end = js.find('"', pos);
    if (end == std::string::npos) break;
    std::string url = js.substr(pos, end - pos);
    // 只看 .exe 资产（完整安装包），跳过 "-x86" 变体
    if (url.size() >= 4 && url.compare(url.size() - 4, 4, ".exe") == 0 &&
        url.find("-x86") == std::string::npos) {
      pos = end;
      return Utf8ToWide(url);
    }
    pos = end;
  }
  return std::wstring();
}

// ---- 更新源 URL 列表（主源 + 镜像兜底）：环境变量 > 配置文件 > 默认 GitHub ----
std::vector<std::wstring> LoadUpdateUrls() {
  std::vector<std::wstring> urls;
  // 1) 环境变量（测试/镜像最优先）
  wchar_t env[2048] = {};
  if (::GetEnvironmentVariableW(kEnvUpdateUrl, env, 2048) > 0 && env[0]) {
    urls.push_back(env);
    ::ime::LogF("[updater] 更新源[env]: %ls\n", env);
  }
  // 2) 配置文件 %APPDATA%\Beautype\config\update.yaml
  wchar_t appdata[1024] = {};
  const DWORD alen = ::GetEnvironmentVariableW(L"APPDATA", appdata, 1024);
  if (alen > 0 && alen < 1024) {
    std::wstring cfg = std::wstring(appdata) + L"\\Beautype\\" + kUpdateCfgRel;
    FILE* f = nullptr;
    if (_wfopen_s(&f, cfg.c_str(), L"rb") == 0 && f != nullptr) {
      std::string data;
      char buf[512];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
      fclose(f);
      // 每行非注释、非空的行：普通 URL 行=更新源；"mirror-prefix:" 行=下载镜像前缀（跳过，不当作更新源）
      std::string line;
      auto push_yaml_line = [&](const std::string& ln) {
        if (ln.empty() || ln[0] == '#') return;
        if (ln.find(kMirrorPrefixKey) == 0) return;  // 镜像前缀行归 LoadMirrorPrefixes
        urls.push_back(Utf8ToWide(ln));
        ::ime::LogF("[updater] 更新源[yaml]: %hs\n", ln.c_str());
      };
      for (char c : data) {
        if (c == '\n' || c == '\r') {
          push_yaml_line(line);
          line.clear();
        } else {
          line.push_back(c);
        }
      }
      push_yaml_line(line);
    } else {
      ::ime::LogF("[updater] 读取更新源配置文件失败: %ls\n", cfg.c_str());
    }
  } else {
    ::ime::LogF("[updater] 环境变量 APPDATA 不可用\n");
  }
  // 3) 内置默认 GitHub
  urls.push_back(kDefaultApiUrl);
  ::ime::LogF("[updater] 更新源[default]: %ls\n", kDefaultApiUrl);
  return urls;
}

// ---- 下载镜像前缀（国内网络下载 github.com 资产失败时的兜底通道）----
// 优先级：update.yaml 中 "mirror-prefix: <url>" 行（可多个）> 内置默认列表。
std::vector<std::wstring> LoadMirrorPrefixes() {
  std::vector<std::wstring> out;
  wchar_t appdata[1024] = {};
  const DWORD alen = ::GetEnvironmentVariableW(L"APPDATA", appdata, 1024);
  if (alen > 0 && alen < 1024) {
    std::wstring cfg = std::wstring(appdata) + L"\\Beautype\\" + kUpdateCfgRel;
    FILE* f = nullptr;
    if (_wfopen_s(&f, cfg.c_str(), L"rb") == 0 && f != nullptr) {
      std::string data;
      char buf[512];
      size_t n;
      while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
      fclose(f);
      std::string line;
      auto push_mirror = [&](const std::string& ln) {
        size_t pos = ln.find(kMirrorPrefixKey);
        if (pos == std::string::npos) return;
        std::string val = ln.substr(pos + strlen(kMirrorPrefixKey));
        // 去掉首尾空白
        size_t b = val.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return;
        size_t e = val.find_last_not_of(" \t\r\n");
        val = val.substr(b, e - b + 1);
        if (!val.empty()) {
          out.push_back(Utf8ToWide(val));
          ::ime::LogF("[updater] 镜像前缀[yaml]: %hs\n", val.c_str());
        }
      };
      for (char c : data) {
        if (c == '\n' || c == '\r') {
          push_mirror(line);
          line.clear();
        } else {
          line.push_back(c);
        }
      }
      push_mirror(line);
    }
  }
  // 内置默认镜像（yaml 配置在前，内置默认在后兜底）
  for (int i = 0; i < kDefaultMirrorCount; ++i) {
    out.push_back(kDefaultMirrorPrefixes[i]);
    ::ime::LogF("[updater] 镜像前缀[default]: %ls\n", kDefaultMirrorPrefixes[i]);
  }
  return out;
}

// ---- WinHTTP 同步 GET（响应体 UTF-8 → wstring；失败返回空串） ----
std::wstring HttpGet(const std::wstring& url) {
  HINTERNET hSession = ::WinHttpOpen(L"BeautypeUpdater/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    ::ime::LogF("[updater] WinHttpOpen 失败 err=%lu\n", ::GetLastError());
    return std::wstring();
  }
  HINTERNET hConnect = nullptr;
  HINTERNET hRequest = nullptr;
  std::wstring result;

  URL_COMPONENTSW comp{};
  comp.dwStructSize = sizeof(comp);
  // WinHttpCrackUrl 要求组件指针指向调用方缓冲区（指针为 NULL 时只返回长度、
  // 不复制内容——旧实现全传 nullptr，lpszHostName/lpszUrlPath 为空指针，
  // wstring(nullptr, n) 构造出垃圾 host → WinHttpConnect 失败 → 永远连不上）。
  wchar_t scheme_buf[32] = {}, host_buf[512] = {}, path_buf[4096] = {};
  wchar_t extra_buf[512] = {};
  comp.lpszScheme = scheme_buf;
  comp.dwSchemeLength = 32;
  comp.lpszHostName = host_buf;
  comp.dwHostNameLength = 512;
  comp.lpszUrlPath = path_buf;
  comp.dwUrlPathLength = 4096;
  comp.lpszExtraInfo = extra_buf;
  comp.dwExtraInfoLength = 512;
  if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &comp)) {
    ::ime::LogF("[updater] WinHttpCrackUrl 失败 err=%lu url=%ls\n", ::GetLastError(),
                url.c_str());
    ::WinHttpCloseHandle(hSession);
    return std::wstring();
  }
  std::wstring host(host_buf);
  std::wstring path(path_buf);
  if (extra_buf[0]) {
    path.append(extra_buf);
  }
  // 默认端口按协议：http=80、https=443（旧实现无端口 URL 一律 443，http 源会连错）
  INTERNET_PORT port = comp.nPort
                           ? comp.nPort
                           : (comp.nScheme == INTERNET_SCHEME_HTTPS ? 443 : 80);
  hConnect = ::WinHttpConnect(hSession, host.c_str(), port, 0);
  if (hConnect) {
    hRequest = ::WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    comp.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
  } else {
    ::ime::LogF("[updater] WinHttpConnect 失败 err=%lu host=%ls port=%u\n",
                ::GetLastError(), host.c_str(), port);
  }
  if (hRequest) {
    if (::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                             WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        ::WinHttpReceiveResponse(hRequest, nullptr)) {
      DWORD status = 0, len = sizeof(status);
      ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                            WINHTTP_NO_HEADER_INDEX);
      if (status == 200) {
        std::string data;
        DWORD avail = 0, total = 0;
        do {
          avail = 0;
          if (!::WinHttpQueryDataAvailable(hRequest, &avail)) break;
          if (avail == 0) break;
          std::vector<char> buf(avail);
          DWORD read = 0;
          if (!::WinHttpReadData(hRequest, buf.data(), avail, &read)) break;
          data.append(buf.data(), read);
          total += read;
          if (total > 8 * 1024 * 1024) break;  // 上限 8MB（release JSON 远小于此）
        } while (avail > 0);
        ::ime::LogF("[updater] GET %ls → 200, %zu bytes\n", url.c_str(), total);
        result = Utf8ToWide(data);
      } else {
        ::ime::LogF("[updater] GET %ls → HTTP %lu\n", url.c_str(), status);
      }
    } else {
      ::ime::LogF("[updater] WinHttpSend/Receive 失败 err=%lu url=%ls\n",
                  ::GetLastError(), url.c_str());
    }
    ::WinHttpCloseHandle(hRequest);
  }
  if (hConnect) ::WinHttpCloseHandle(hConnect);
  ::WinHttpCloseHandle(hSession);
  return result;
}

// ---- WinHTTP 流式下载到文件（与 HttpGet 同网络栈；返回 true=HTTP 200 且写出文件）----
// 曾用 URLDownloadToFileW（UrlMon），在部分机器/代理残留环境下对所有 https 一律返回
// 0x800c0005（INET_E_UNRECOGNIZED_SCHEME）导致下载全挂；改走 WinHTTP 与检查 API 同栈，
// 国内网络下 GitHub 直连被重置时镜像兜底同样可用（实测 gh-proxy 直连正常）。
bool HttpDownloadToFile(const std::wstring& url, const std::wstring& dst) {
  HINTERNET hSession = ::WinHttpOpen(L"BeautypeUpdater/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    ::ime::LogF("[updater] 下载 WinHttpOpen 失败 err=%lu\n", ::GetLastError());
    return false;
  }
  bool ok = false;
  URL_COMPONENTSW comp{};
  comp.dwStructSize = sizeof(comp);
  wchar_t scheme_buf[32] = {}, host_buf[512] = {}, path_buf[4096] = {};
  wchar_t extra_buf[512] = {};
  comp.lpszScheme = scheme_buf;
  comp.dwSchemeLength = 32;
  comp.lpszHostName = host_buf;
  comp.dwHostNameLength = 512;
  comp.lpszUrlPath = path_buf;
  comp.dwUrlPathLength = 4096;
  comp.lpszExtraInfo = extra_buf;
  comp.dwExtraInfoLength = 512;
  if (!::WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &comp)) {
    ::ime::LogF("[updater] 下载 WinHttpCrackUrl 失败 err=%lu url=%ls\n",
                ::GetLastError(), url.c_str());
    ::WinHttpCloseHandle(hSession);
    return false;
  }
  std::wstring host(host_buf);
  std::wstring path(path_buf);
  if (extra_buf[0]) path.append(extra_buf);
  INTERNET_PORT port = comp.nPort
                           ? comp.nPort
                           : (comp.nScheme == INTERNET_SCHEME_HTTPS ? 443 : 80);
  HINTERNET hConnect = ::WinHttpConnect(hSession, host.c_str(), port, 0);
  if (hConnect) {
    HINTERNET hRequest = ::WinHttpOpenRequest(
        hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        comp.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (hRequest) {
      if (::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
          ::WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        ::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                              WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
          HANDLE hFile = ::CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
          if (hFile != INVALID_HANDLE_VALUE) {
            ok = true;
            DWORD total = 0;
            DWORD avail = 0;
            do {
              avail = 0;
              if (!::WinHttpQueryDataAvailable(hRequest, &avail)) {
                ok = false;
                break;
              }
              if (avail == 0) break;
              std::vector<char> buf(avail);
              DWORD read = 0;
              if (!::WinHttpReadData(hRequest, buf.data(), avail, &read)) {
                ok = false;
                break;
              }
              DWORD written = 0;
              if (!::WriteFile(hFile, buf.data(), read, &written, nullptr) ||
                  written != read) {
                ok = false;
                break;
              }
              total += read;
            } while (avail > 0);
            ::CloseHandle(hFile);
            if (!ok) ::DeleteFileW(dst.c_str());
            ::ime::LogF("[updater] 下载 %ls → 200, %u bytes\n", url.c_str(), total);
          } else {
            ::ime::LogF("[updater] 下载创建文件失败 err=%lu dst=%ls\n",
                        ::GetLastError(), dst.c_str());
          }
        } else {
          ::ime::LogF("[updater] 下载 %ls → HTTP %lu\n", url.c_str(), status);
        }
      } else {
        ::ime::LogF("[updater] 下载 WinHttpSend/Receive 失败 err=%lu url=%ls\n",
                    ::GetLastError(), url.c_str());
      }
      ::WinHttpCloseHandle(hRequest);
    } else {
      ::ime::LogF("[updater] 下载 WinHttpOpenRequest 失败 err=%lu\n", ::GetLastError());
    }
  } else {
    ::ime::LogF("[updater] 下载 WinHttpConnect 失败 err=%lu host=%ls port=%u\n",
                ::GetLastError(), host.c_str(), port);
  }
  if (hConnect) ::WinHttpCloseHandle(hConnect);
  ::WinHttpCloseHandle(hSession);
  return ok;
}

// ---- 文件 SHA256（CryptAPI） ----
std::wstring FileSha256(const std::wstring& path) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::wstring();
  HCRYPTPROV prov = 0;
  HCRYPTHASH hash = 0;
  std::wstring hex;
  if (::CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT) &&
      ::CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
    BYTE buf[64 * 1024];
    DWORD n = 0;
    while (::ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
      ::CryptHashData(hash, buf, n, 0);
    }
    BYTE digest[32];
    DWORD dlen = sizeof(digest);
    if (::CryptGetHashParam(hash, HP_HASHVAL, digest, &dlen, 0)) {
      wchar_t tmp[3] = {};
      for (DWORD i = 0; i < dlen; ++i) {
        swprintf_s(tmp, L"%02x", digest[i]);
        hex += tmp;
      }
    }
    ::CryptDestroyHash(hash);
  }
  if (prov) ::CryptReleaseContext(prov, 0);
  ::CloseHandle(h);
  return hex;
}

// ---- 状态（跨线程：owner 窗口句柄 + 当前 release 信息） ----
volatile HWND g_owner = nullptr;
ReleaseInfo g_release;

void PostMsg(UINT msg, WPARAM wp = 0) {
  HWND h = g_owner;
  if (h) ::PostMessageW(h, msg, wp, 0);
}

// ---- 下载线程：URLDownloadToFile → 校验 → 通知主线程 ----
DWORD WINAPI DownloadThread(LPVOID) {
  wchar_t tmp[MAX_PATH] = {};
  ::GetTempPathW(MAX_PATH, tmp);
  std::wstring dst = std::wstring(tmp) + kTempInstaller;
  // 下载候选：原 URL（GitHub 直连）优先；国内网络常被重置，失败则依次尝试镜像前缀
  std::vector<std::wstring> candidates;
  candidates.push_back(g_release.exeUrl);
  const std::vector<std::wstring> mirrors = LoadMirrorPrefixes();
  for (const auto& m : mirrors) {
    candidates.push_back(m + g_release.exeUrl);
  }
  ::ime::LogF("[updater] 下载候选 %zu 个（首个为 GitHub 直连）\n", candidates.size());
  for (const auto& url : candidates) {
    ::DeleteFileW(dst.c_str());
    const bool dl_ok = HttpDownloadToFile(url, dst);
    ::ime::LogF("[updater] 下载尝试: %ls → %s\n", url.c_str(),
                dl_ok ? L"OK" : L"FAIL");
    if (!dl_ok) continue;
    // 校验：非空 + （可选）SHA256 强校验
    bool ok = false;
    {
      HANDLE h = ::CreateFileW(dst.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        if (::GetFileSizeEx(h, &size) && size.QuadPart > 0) ok = true;
        ::CloseHandle(h);
      }
    }
    if (ok && !g_release.sha256.empty()) {
      std::wstring got = FileSha256(dst);
      // 大小写无关比较
      std::wstring want = g_release.sha256;
      for (auto& c : got) c = towlower(c);
      for (auto& c : want) c = towlower(c);
      if (got != want) ok = false;
      ::ime::LogF("[updater] SHA256 got=%ls want=%ls → %hs\n", got.c_str(),
                  want.c_str(), ok ? "OK" : "不匹配");
    }
    if (ok) {
      ::ime::LogF("[updater] 下载并校验成功: %ls\n", url.c_str());
      PostMsg(kMsgUpdateReady, 0);
      return 0;
    }
    ::ime::LogF("[updater] 该候选失败，尝试下一个下载通道\n");
  }
  ::DeleteFileW(dst.c_str());
  PostMsg(kMsgUpdateFail);
  return 0;
}

// ---- 检查线程：主源 + 镜像兜底，逐个尝试 ----
DWORD WINAPI CheckThread(LPVOID) {
  const std::vector<std::wstring> urls = LoadUpdateUrls();
  const std::wstring localVer = Utf8ToWide(BEAUTYPE_VERSION);
  ::ime::LogF("[updater] 检查开始 本地版本=%ls 更新源数=%zu\n", localVer.c_str(),
              urls.size());
  ReleaseInfo found;
  std::wstring lastErr;
  for (const std::wstring& url : urls) {
    ReleaseInfo info;
    const std::wstring json = HttpGet(url);
    if (json.empty()) {
      lastErr = L"网络请求失败（更新源不可达）";
      ::ime::LogF("[updater] 源 %ls 请求失败: %ls\n", url.c_str(), lastErr.c_str());
      continue;
    }
    info.tag = ExtractJsonStr(json, "tag_name");
    info.exeUrl = FindExeDownloadUrl(json);
    info.sha256 = ExtractSha256(json);
    ::ime::LogF("[updater] 源 %ls 响应 tag=%ls exe=%ls sha=%ls\n", url.c_str(),
                info.tag.c_str(), info.exeUrl.c_str(), info.sha256.c_str());
    if (info.tag.empty() || info.exeUrl.empty()) {
      lastErr = L"更新源响应格式异常";
      continue;
    }
    // 只跟正式主线：GitHub /releases/latest 本身跳过 prerelease；
    // 镜像源若给出 prerelease，靠版本比较（pre-release 版本号一般更高）兜底不升级。
    if (CompareVersion(info.tag, localVer) > 0) {
      found = info;
      break;
    }
    // 无新版：主源即可判定，不需要再试镜像
    PostMsg(kMsgUpdateDone);
    return 0;
  }
  if (found.valid || !found.tag.empty()) {
    g_release = found;
    ::ime::LogF("[updater] 发现新版 %ls，请求用户确认\n", found.tag.c_str());
    PostMsg(kMsgUpdateFound);
  } else {
    ::ime::LogF("[updater] 全部更新源失败（%ls），弹失败提示\n", lastErr.c_str());
    PostMsg(kMsgUpdateFail);  // 全部源失败
  }
  return 0;
}

}  // namespace

void StartCheckUpdate(HWND owner) {
  g_owner = owner;
  ::CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr);
}

void HandleUpdateMessage(HWND owner, UINT msg) {
  switch (msg) {
    case kMsgUpdateFound: {
      // 发现新版：弹窗确认（用户拍板交互）
      std::wstring text = L"发现新版本 " + g_release.tag +
                          L"，是否下载并安装？\n\n（下载来源：GitHub Releases，"
                          L"国内网络自动切换镜像兜底）";
      if (::MessageBoxW(owner, text.c_str(), L"Beautype 检查更新",
                        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES) {
        ::CreateThread(nullptr, 0, DownloadThread, nullptr, 0, nullptr);
      }
      break;
    }
    case kMsgUpdateReady: {
      wchar_t tmp[MAX_PATH] = {};
      ::GetTempPathW(MAX_PATH, tmp);
      std::wstring inst = std::wstring(tmp) + kTempInstaller;
      std::wstring text =
          L"新版本已下载完成（" + g_release.tag + L"），是否立即运行安装程序？";
      if (::MessageBoxW(owner, text.c_str(), L"Beautype 检查更新",
                        MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES) {
        // 运行安装器（安装向导；装完注册表 Run 项拉起新版 server）
        ::ShellExecuteW(owner, L"open", inst.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      } else {
        // 保留安装包到 %TEMP%，用户可稍后手动安装
      }
      break;
    }
    case kMsgUpdateFail:
      ::MessageBoxW(owner, L"检查更新失败：无法获取最新版本信息。\n请检查网络后重试。",
                    L"Beautype 检查更新", MB_OK | MB_ICONWARNING | MB_TOPMOST);
      break;
    case kMsgUpdateDone:
    default:
      ::MessageBoxW(owner, L"已是最新版本。", L"Beautype 检查更新",
                    MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
      break;
  }
}

}  // namespace ime::updater
