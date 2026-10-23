#include "theme.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

// windows.h 的 min/max 宏会破坏标准库模板（yaml-cpp 等头文件可能使用）
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <yaml-cpp/yaml.h>
#include "log/log.h"

// 统一日志文件（rime_server.exe 内为 rime_server.log，rime_tsf.dll 内为
// rime_tsf.log；由各二进制内的 ime::g_log 定义提供；正式版无 stdout）
namespace ime { extern FILE* g_log; }

namespace ime::skin {

Theme DefaultTheme() { return Theme{}; }

namespace {

// 宽字符路径 → UTF-8（background_image 解析后用，candidate_window 再转回宽）
std::string WidenPathToUtf8(const std::wstring& ws) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 1 ? static_cast<std::size_t>(n - 1) : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, nullptr, nullptr);
  }
  return s;
}

// 解析背景图路径：绝对路径原样用；相对路径以 theme.yaml 所在目录为基准
// （用户把 PNG 与 theme.yaml 放同一配置目录，填文件名即可）。
std::string ResolveImagePath(const std::string& p, const std::string& yaml_path) {
  const std::filesystem::path raw(p);
  if (p.empty()) return "";  // 空字符串直接返回空，不要解析成目录路径！
  if (raw.is_absolute()) {
    return p;
  }
  const std::filesystem::path base = std::filesystem::path(yaml_path).parent_path();
  return WidenPathToUtf8((base / raw).wstring());
}

// YAML 双引号标量内反斜杠是转义符（\U/\u/\x 等）：Windows 绝对路径
// C:\Users\... 里的 \U 会被 yaml-cpp 当 unicode 转义而解析失败
// （实测报 bad character found while scanning hex number）。
// 写入双引号字符串前必须把 \ 转义为 \\（读回时 yaml-cpp 自动还原）。
std::string EscapeYamlString(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\') {
      out += "\\\\";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

}  // namespace

std::uint32_t ParseHexColor(const std::string& s) {
  // 接受 #RGB / #RRGGBB / #AARRGGBB（# 可省略）
  std::string hex;
  for (char ch : s) {
    if (ch == '#') {
      continue;
    }
    hex.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
  };
  auto value = [&](std::size_t i) -> int {
    if (i + 1 >= hex.size()) return -1;
    int hi = nibble(hex[i]);
    int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
  };
  if (hex.size() == 3) {
    // 简写 #ABC → #AABBCC
    int r = value(0), g = value(1), b = value(2);
    if (r < 0 || g < 0 || b < 0) return 0xFF000000;
    r = (r << 4) | r;
    g = (g << 4) | g;
    b = (b << 4) | b;
    return 0xFF000000u | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
  }
  if (hex.size() == 6) {
    int r = value(0), g = value(2), b = value(4);
    if (r < 0 || g < 0 || b < 0) return 0xFF000000;
    return 0xFF000000u | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
  }
  if (hex.size() == 8) {
    int a = value(0), r = value(2), g = value(4), b = value(6);
    if (a < 0 || r < 0 || g < 0 || b < 0) return 0xFF000000;
    return (static_cast<std::uint32_t>(a) << 24) | (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
  }
  return 0xFF000000;
}

std::string HexColor(std::uint32_t argb) {
  char buf[9];
  std::snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned int>(argb));
  return std::string(buf);
}

bool WriteThemeToYaml(const std::string& path, const Theme& theme) {
  // 中文配置名/路径修复：UTF-8 窄字符串路径必须用 std::filesystem::u8path
  // 转成正确的宽字符路径。MSVC 的 std::filesystem::path(std::string) / ofstream
  // 默认按系统 ANSI 代码页(GBK) 解释 UTF-8 字节，含中文的路径会变成乱码/无效
  // 路径 → 抛未捕获 C++ 异常 → std::terminate → 0xc0000409 崩溃（真机铁证：
  // 保存中文配置名「bt兔」反复闪退，三个崩溃转储调用链一致；英文名正常）。
  const std::filesystem::path fs_path = std::filesystem::u8path(path);
  // 目录缺失时自动创建（真机根因 2026-10-03：清理旧配置目录后 config 目录不存在，
  // ofstream 打开失败 → 设置面板「写入 theme.yaml 失败，请检查配置目录权限」）。
  {
    const std::filesystem::path parent = fs_path.parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        ::ime::LogF( "[skin] 创建主题目录失败 %s: %s\n", parent.string().c_str(),
                    ec.message().c_str());
        return false;
      }
    }
  }
  std::ofstream out(fs_path, std::ios::trunc);
  if (!out) {
    ::ime::LogF( "[skin] 无法写入主题文件 %s\n", path.c_str());
    return false;
  }
  // 模板含中文注释（UTF-8）；Windows 记事本可正常编辑保存。
  // 颜色/背景图字符串一律加引号：# 开头不加引号会被 yaml 当成注释。
  out << "# Beautype 候选窗主题配置（设置面板「保存并应用」或重启 rime_ime.exe 生效）\n";
  out << "# 颜色格式：#RRGGBB 或 #AARRGGBB（alpha 默认 FF 不透明）\n";
  out << "# 背景图：把 PNG 放到本文件同目录，background_image 填文件名（相对本目录）；\n";
  out << "#   空字符串 = 纯色背景。圆角：highlight_radius 像素（0 = 直角）。\n";
  out << "name: " << theme.name << "\n";
  out << "colors:\n";
  out << "  background: \"#" << HexColor(theme.colors.background) << "\"\n";
  out << "  text: \"#" << HexColor(theme.colors.text) << "\"\n";
  out << "  highlight_text: \"#" << HexColor(theme.colors.highlight_text) << "\"\n";
  out << "  highlight_background: \"#" << HexColor(theme.colors.highlight_background) << "\"\n";
  out << "font:\n";
  out << "  family: \"" << EscapeYamlString(theme.font.family) << "\"\n";
  out << "  size: " << theme.font.size << "\n";
  out << "layout:\n";
  out << "  padding_x: " << theme.layout.padding_x << "\n";
  out << "  padding_y: " << theme.layout.padding_y << "\n";
  out << "  item_spacing: " << theme.layout.item_spacing << "\n";
  out << "  item_height: " << theme.layout.item_height << "\n";
  out << "  max_items: " << theme.layout.max_items << "\n";
  out << "background_image: \"" << EscapeYamlString(theme.background_image) << "\"\n";
  out << "highlight_radius: " << theme.highlight_radius << "\n";
  // v3 皮肤块：mode=None 时也写出（便于用户手改，加载时缺省兼容）
  {
    const char* mode_str = "none";
    if (theme.skin.mode == SkinMode::Role) {
      mode_str = "role";
    } else if (theme.skin.mode == SkinMode::Frame) {
      mode_str = "frame";
    }
    out << "skin:\n";
    out << "  mode: " << mode_str << "\n";
    out << "  role_image: \"" << EscapeYamlString(theme.skin.role_image) << "\"\n";
    out << "  role_height: " << theme.skin.role_height << "\n";
    out << "  frame_image: \"" << EscapeYamlString(theme.skin.frame_image) << "\"\n";
    out << "  borders:\n";
    out << "    left: " << theme.skin.borders.left << "\n";
    out << "    right: " << theme.skin.borders.right << "\n";
    out << "    top: " << theme.skin.borders.top << "\n";
    out << "    bottom: " << theme.skin.borders.bottom << "\n";
    out << "  text_region:\n";
    out << "    x: " << theme.skin.text_region.x << "\n";
    out << "    y: " << theme.skin.text_region.y << "\n";
    out << "    width: " << theme.skin.text_region.width << "\n";
    out << "    height: " << theme.skin.text_region.height << "\n";
  }
  out.close();
  return true;
}

bool LoadThemeFromYaml(const std::string& path, Theme* out) {
  if (out == nullptr) {
    return false;
  }
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const YAML::Exception& e) {
    ::ime::LogF( "[skin] theme.yaml 解析失败: %s\n", e.what());
    return false;
  }
  if (!root || !root.IsMap()) {
    ::ime::LogF( "[skin] theme.yaml 顶层不是映射\n");
    return false;
  }

  Theme t = DefaultTheme();
  if (root["name"]) {
    t.name = root["name"].as<std::string>();
  }
  if (root["colors"] && root["colors"].IsMap()) {
    if (root["colors"]["background"]) t.colors.background = ParseHexColor(root["colors"]["background"].as<std::string>());
    if (root["colors"]["text"]) t.colors.text = ParseHexColor(root["colors"]["text"].as<std::string>());
    if (root["colors"]["highlight_text"]) t.colors.highlight_text = ParseHexColor(root["colors"]["highlight_text"].as<std::string>());
    if (root["colors"]["highlight_background"]) t.colors.highlight_background = ParseHexColor(root["colors"]["highlight_background"].as<std::string>());
  }
  if (root["font"] && root["font"].IsMap()) {
    if (root["font"]["family"]) t.font.family = root["font"]["family"].as<std::string>();
    if (root["font"]["size"]) t.font.size = root["font"]["size"].as<int>();
  }
  if (root["layout"] && root["layout"].IsMap()) {
    if (root["layout"]["padding_x"]) t.layout.padding_x = root["layout"]["padding_x"].as<int>();
    if (root["layout"]["padding_y"]) t.layout.padding_y = root["layout"]["padding_y"].as<int>();
    if (root["layout"]["item_spacing"]) t.layout.item_spacing = root["layout"]["item_spacing"].as<int>();
    if (root["layout"]["item_height"]) t.layout.item_height = root["layout"]["item_height"].as<int>();
    if (root["layout"]["max_items"]) t.layout.max_items = root["layout"]["max_items"].as<int>();
  }
  // PNG 皮肤扩展点字段：一期通常缺省；未来 importer 产出后由 UI 消费。
  // background_image 相对路径以 theme.yaml 所在目录为基准解析。
  if (root["background_image"]) t.background_image = ResolveImagePath(root["background_image"].as<std::string>(), path);
  if (root["border_image"]) t.border_image = root["border_image"].as<std::string>();
  if (root["highlight_radius"]) t.highlight_radius = root["highlight_radius"].as<int>();

  // v3 皮肤块（mode/role_image/frame_image/borders/text_region；缺省 = None）
  if (root["skin"] && root["skin"].IsMap()) {
    const YAML::Node& s = root["skin"];
    if (s["mode"]) {
      const std::string mode = s["mode"].as<std::string>();
      if (mode == "role") {
        t.skin.mode = SkinMode::Role;
      } else if (mode == "frame") {
        t.skin.mode = SkinMode::Frame;
      } else {
        t.skin.mode = SkinMode::None;
      }
    }
    if (s["role_image"]) t.skin.role_image = ResolveImagePath(s["role_image"].as<std::string>(), path);
    if (s["role_height"]) t.skin.role_height = s["role_height"].as<int>();
    if (s["frame_image"]) t.skin.frame_image = ResolveImagePath(s["frame_image"].as<std::string>(), path);
    if (s["borders"] && s["borders"].IsMap()) {
      const YAML::Node& b = s["borders"];
      if (b["left"]) t.skin.borders.left = b["left"].as<int>();
      if (b["right"]) t.skin.borders.right = b["right"].as<int>();
      if (b["top"]) t.skin.borders.top = b["top"].as<int>();
      if (b["bottom"]) t.skin.borders.bottom = b["bottom"].as<int>();
    }
    if (s["text_region"] && s["text_region"].IsMap()) {
      const YAML::Node& tr = s["text_region"];
      // v4.6：兼容旧 float（0-1 相对比例）和新 int（绝对像素）配置。
      // 旧 float 值 < 1.0 → 钳制为 0（让 ResolveTextRegion 用默认推导），
      // 避免 as<int>() 对 float 值抛出 BadConversion 异常导致 rime_server 崩溃。
      auto read_int = [](const YAML::Node& n) -> int {
        if (!n) return 0;
        double v = n.as<double>(0.0);
        if (v < 1.0) return 0;  // 旧 float 相对比例 → 钳制为 0
        return static_cast<int>(v);
      };
      t.skin.text_region.x = read_int(tr["x"]);
      t.skin.text_region.y = read_int(tr["y"]);
      t.skin.text_region.width = read_int(tr["width"]);
      t.skin.text_region.height = read_int(tr["height"]);
    }
  }

  *out = std::move(t);
  return true;
}

}  // namespace ime::skin
