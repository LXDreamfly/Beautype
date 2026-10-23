// fs_util.h —— 中文路径安全的文件系统辅助。
//
// 背景（本机实测 2026-10-04）：MSVC 14.44（VS2022 Build Tools）的
// std::filesystem::is_directory / is_regular_file 等 status 类接口对含中文的
// std::string 路径存在 CRT 级崩溃（0xc0000409 = __fastfail，abort 路径），
// /O2 /O1 /Od 全部复现；同一编译器下 exists / create_directories /
// remove_all 对中文路径实测正常。此外 std::filesystem::path::string() 与
// std::getenv 在 Windows 返回的是 ANSI（GBK）编码字节，把 GBK 当 UTF-8
// 转换同样会得到错误路径。因此凡可能接触中文路径的"目录/文件存在性、
// 条目枚举、路径比较"一律走 Win32 W 系 API；优先使用 *W 宽字符直通版本
// （调用方用 .wstring() / _wgetenv 拿到正确的 UTF-16 后直接传入），
// 只有确保证为 UTF-8 的 std::string 才走 UTF-8 包装版。
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>

namespace ime {

// UTF-8 std::string → UTF-16 宽字符串（MultiByteToWideChar, CP_UTF8）
inline std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) {
    return L"";
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) {
    return L"";
  }
  std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  return w;
}

// UTF-16 宽字符串 → UTF-8 std::string（WideCharToMultiByte, CP_UTF8）
inline std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) {
    return "";
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) {
    return "";
  }
  std::string s(static_cast<std::size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
  return s;
}

// 路径存在且为目录（GetFileAttributesW；中文路径安全）。宽字符直通版。
inline bool DirectoryExistsW(const std::wstring& w) {
  if (w.empty()) {
    return false;
  }
  const DWORD attr = GetFileAttributesW(w.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// 路径存在且为普通文件（GetFileAttributesW；中文路径安全）。宽字符直通版。
inline bool FileExistsW(const std::wstring& w) {
  if (w.empty()) {
    return false;
  }
  const DWORD attr = GetFileAttributesW(w.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// 目录首层条目名列表（FindFirstFileW；中文路径安全；替代 directory_iterator）。
// 失败或目录不存在时返回空列表。宽字符直通版。
inline std::vector<std::wstring> ListDirectoryEntriesW(const std::wstring& wdir) {
  std::vector<std::wstring> names;
  if (wdir.empty()) {
    return names;
  }
  std::wstring pattern = wdir;
  if (pattern.back() != L'\\' && pattern.back() != L'/') {
    pattern += L'\\';
  }
  pattern += L'*';
  WIN32_FIND_DATAW fd = {};
  HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return names;
  }
  do {
    if (wcscmp(fd.cFileName, L".") != 0 && wcscmp(fd.cFileName, L"..") != 0) {
      names.emplace_back(fd.cFileName);
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return names;
}

// 两个路径指向同一文件：规范化绝对路径后不区分大小写比较（中文路径安全）。
// 语义比 std::filesystem::equivalent 略宽松（不解析符号链接/硬链接），
// 用于"上传文件与目标文件是否同一路径"的覆盖保护，够用即可。宽字符直通版。
inline bool PathsEquivalentW(const std::wstring& wa, const std::wstring& wb) {
  auto norm = [](const std::wstring& w) -> std::wstring {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetFullPathNameW(w.c_str(), MAX_PATH, buf, nullptr);
    if (n == 0 || n >= MAX_PATH) {
      return w;
    }
    std::wstring r(buf, n);
    for (auto& ch : r) {
      ch = towlower(ch);
    }
    return r;
  };
  return norm(wa) == norm(wb);
}

// ---- UTF-8 包装版：仅在调用方确保证为 UTF-8 编码时使用 ----
inline bool DirectoryExists(const std::string& utf8_path) {
  return DirectoryExistsW(Utf8ToWide(utf8_path));
}

inline bool FileExists(const std::string& utf8_path) {
  return FileExistsW(Utf8ToWide(utf8_path));
}

inline std::vector<std::string> ListDirectoryEntries(const std::string& utf8_dir) {
  std::vector<std::string> names;
  for (const std::wstring& w : ListDirectoryEntriesW(Utf8ToWide(utf8_dir))) {
    names.push_back(WideToUtf8(w));
  }
  return names;
}

inline bool PathsEquivalent(const std::string& utf8_a, const std::string& utf8_b) {
  return PathsEquivalentW(Utf8ToWide(utf8_a), Utf8ToWide(utf8_b));
}

// ---- 递归复制目录（中文路径安全；宽字符直通版） ----
// exclude：要跳过的顶层条目名（如同步时跳过历史 sync 目录，避免嵌套膨胀）。
// 目标目录不存在则创建；返回 false = 复制中断（目标可能不完整）。
inline bool CopyDirectoryRecursiveW(const std::wstring& src,
                                    const std::wstring& dst,
                                    const std::vector<std::wstring>& exclude = {}) {
  if (src.empty() || dst.empty() || PathsEquivalentW(src, dst)) {
    return false;
  }
  if (!CreateDirectoryW(dst.c_str(), nullptr) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    return false;
  }
  for (const std::wstring& name : ListDirectoryEntriesW(src)) {
    if (std::find(exclude.begin(), exclude.end(), name) != exclude.end()) {
      continue;
    }
    const std::wstring s = src + L"\\" + name;
    const std::wstring d = dst + L"\\" + name;
    const DWORD attr = GetFileAttributesW(s.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
      return false;
    }
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      if (!CopyDirectoryRecursiveW(s, d, exclude)) {
        return false;
      }
    } else if (!CopyFileW(s.c_str(), d.c_str(), FALSE)) {
      return false;
    }
  }
  return true;
}

inline bool CopyDirectoryRecursive(const std::string& utf8_src,
                                   const std::string& utf8_dst,
                                   const std::vector<std::string>& exclude = {}) {
  std::vector<std::wstring> wex;
  for (const std::string& e : exclude) {
    wex.push_back(Utf8ToWide(e));
  }
  return CopyDirectoryRecursiveW(Utf8ToWide(utf8_src), Utf8ToWide(utf8_dst), wex);
}

}  // namespace ime
