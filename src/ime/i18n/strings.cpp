#include "i18n/strings.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <string>

namespace ime {
namespace i18n {
namespace {

Lang g_lang = Lang::Zh;
bool g_loaded = false;

std::filesystem::path ConfigFilePath() {
  wchar_t appdata[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) {
    return {};
  }
  return std::filesystem::path(appdata) / L"Beautype" / L"config" / L"language";
}

std::string ReadConfigFile(const std::filesystem::path& path) {
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return "";
  }
  char buf[32] = {};
  DWORD read = 0;
  if (!::ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr)) {
    read = 0;
  }
  ::CloseHandle(h);
  return std::string(buf, read);
}

void LoadLanguage() {
  if (g_loaded) {
    return;
  }
  g_loaded = true;
  const std::filesystem::path path = ConfigFilePath();
  if (path.empty()) {
    return;
  }
  std::string body = ReadConfigFile(path);
  std::transform(body.begin(), body.end(), body.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (body.find("en") != std::string::npos) {
    g_lang = Lang::En;
  } else {
    g_lang = Lang::Zh;
  }
}

// 中英文对照表：[ID][0]=中文，[ID][1]=English。
const std::wstring& Table(Str id, Lang lang) {
  static const std::wstring kEmpty;
  static const std::wstring kTable[static_cast<std::size_t>(Str::kCount)][2] = {
      // 品牌 / TSF 显示属性
      {L"Beautype 输入法", L"Beautype IME"},
      {L"Beautype 行内组合", L"Beautype inline composition"},
      {L"Beautype 候选列表", L"Beautype candidate list"},

      // 语言切换菜单
      {L"界面语言", L"Interface Language"},
      {L"中文", L"Chinese"},
      {L"English", L"English"},

      // 语言栏 / 服务托盘共用菜单
      {L"切换到中文输入", L"Switch to Chinese input"},
      {L"切换到英文输入", L"Switch to English input"},
      {L"设置", L"Settings"},
      {L"词典管理", L"Dictionary Manager"},
      {L"用户数据同步", L"Sync User Data"},
      {L"打开桌宠", L"Open Pet"},
      {L"置顶桌宠", L"Pin Pet"},
      {L"打开用户目录", L"Open User Directory"},
      {L"打开日志目录", L"Open Log Directory"},
      {L"重新部署", L"Redeploy"},
      {L"检查更新", L"Check for Updates"},
      {L"退出", L"Exit"},
      {L"方案", L"Schemas"},
      {L"中/英 切换", L"Chinese / English"},

      // 语言栏 tooltip
      {L"Beautype：英文模式（点击切换中文）",
       L"Beautype: English mode (click to switch to Chinese)"},
      {L"Beautype：中文模式（点击切换英文）",
       L"Beautype: Chinese mode (click to switch to English)"},

      // 设置面板
      {L"Beautype 设置", L"Beautype Settings"},
      {L"Beautype 皮肤设置", L"Beautype Skin Settings"},
      {L"配置管理", L"Preset Management"},
      {L"颜 色", L"Colors"},
      {L"背景色", L"Background"},
      {L"高亮文字色", L"Highlight Text"},
      {L"正文色", L"Text"},
      {L"高亮底色", L"Highlight Background"},
      {L"字 体", L"Font"},
      {L"皮肤模式", L"Skin Mode"},
      {L"建议 256×256 PNG 透明底",
       L"Recommended: 256×256 transparent PNG"},
      {L"自动置于候选框右侧，不变形",
       L"Placed on the right of the candidate window without distortion"},
      {L"建议 600×200 PNG，中间拉伸",
       L"Recommended: 600×200 PNG, center stretched"},
      {L"预览", L"Preview"},
      {L"文字区域对齐：拖拽红色虚线框，对准输入框图片里的实际文字位置",
       L"Text region: drag the red dashed box to align with the actual text"},
      {L"X", L"X"},
      {L"Y", L"Y"},
      {L"宽", L"Width"},
      {L"高", L"Height"},
      {L"（无背景图，纯色背景）", L"(No background image, solid color)"},
      {L"背景图: ", L"Background: "},
      {L"PNG 图片 (*.png)\0*.png\0所有文件 (*.*)\0*.*\0\0",
       L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0\0"},

      // 设置面板错误提示
      {L"无法创建配置目录:\n", L"Cannot create configuration folder:\n"},
      {L"无法把背景图复制到配置目录:\n",
       L"Failed to copy background image to configuration folder:\n"},
      {L"无法把图片复制到配置目录:\n",
       L"Failed to copy image to configuration folder:\n"},
      {L"\n\n旧图片可能被输入法占用，请重启输入法后重试。",
       L"\n\nThe old image may be locked by the input method. Restart it and try again."},
      {L"图片无法解码，请重新选择 PNG 图片。",
       L"The image cannot be decoded. Please select a PNG again."},
      {L"角色图尺寸不合规（当前 %d×%d）\n建议 256×256 PNG 透明底，至少 64×64。",
       L"Invalid role image size (%d×%d).\nRecommended: 256×256 transparent PNG, at least 64×64."},
      {L"输入框图片尺寸不合规（当前 %d×%d）\n建议 600×200 PNG"
       L"（左右各 150px 不变形、中间拉伸），至少 320×100。",
       L"Invalid frame image size (%d×%d).\nRecommended: 600×200 PNG (150px on each "
       L"side unchanged, center stretched), at least 320×100."},
      {L"写入 theme.yaml 失败，请检查配置目录权限。",
       L"Failed to write theme.yaml. Check the configuration folder permissions."},
      {L"请先输入配置名（例如：日常深色）。",
       L"Enter a preset name first (e.g., Dark Daily)."},
      {L"无法创建配置目录 presets/。", L"Cannot create the presets/ folder."},
      {L"保存配置失败，请检查 presets/ 目录权限。",
       L"Failed to save preset. Check presets/ folder permissions."},
      {L"请先在下拉框选择一个配置。",
       L"Select a preset from the dropdown first."},
      {L"加载配置失败：文件缺失或格式错误。",
       L"Failed to load preset: file missing or invalid format."},
      {L"删除配置失败。", L"Failed to delete preset."},

      // 设置面板按钮 / 单选
      {L"加载", L"Load"},
      {L"删除", L"Delete"},
      {L"保存当前配置", L"Save Current Preset"},
      {L"选择字体…", L"Choose Font…"},
      {L"角色图片", L"Role Image"},
      {L"输入框图片", L"Frame Image"},
      {L"选择图片…", L"Choose Image…"},
      {L"打开目录", L"Open Folder"},
      {L"恢复默认", L"Restore Defaults"},
      {L"保存并应用", L"Save & Apply"},
      {L"关闭", L"Close"},

      // 取色器
      {L"选择颜色", L"Select Color"},
      {L"吸取屏幕颜色", L"Pick Screen Color"},
      {L"确定", L"OK"},
      {L"取消", L"Cancel"},
      {L"取色中：移动鼠标预览，左键确认，Esc 取消",
       L"Picking: move mouse to preview, left-click to confirm, Esc to cancel"},

      // 更新器
      {L"Beautype 检查更新", L"Beautype Check for Updates"},
      {L"发现新版本 ", L"New version "},
      {L"，是否下载并安装？\n\n（下载来源：GitHub Releases，"
       L"国内网络自动切换镜像兜底）",
       L" is available. Download and install?\n\n(Source: GitHub Releases; "
       L"mirrors are used automatically on slow networks.)"},
      {L"新版本已下载完成（", L"New version ("},
      {L"），是否立即运行安装程序？", L") downloaded. Run the installer now?"},
      {L"检查更新失败：无法获取最新版本信息。\n请检查网络后重试。",
       L"Update check failed: cannot retrieve latest version information.\n"
       L"Check your network and try again."},
      {L"已是最新版本。", L"You are up to date."},
  };

  const int index = static_cast<int>(lang);
  const int enum_id = static_cast<int>(id);
  if (enum_id < 0 || enum_id >= static_cast<int>(Str::kCount) ||
      index < 0 || index > 1) {
    return kEmpty;
  }
  return kTable[enum_id][index];
}

}  // namespace

Lang CurrentLanguage() {
  LoadLanguage();
  return g_lang;
}

bool IsEnglish() {
  return CurrentLanguage() == Lang::En;
}

void ReloadLanguage() {
  g_loaded = false;
  LoadLanguage();
}

void SetLanguage(Lang lang, bool persist) {
  g_lang = lang;
  g_loaded = true;
  if (!persist) {
    return;
  }
  const std::filesystem::path path = ConfigFilePath();
  if (path.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  const char* body = (lang == Lang::En) ? "en\n" : "zh\n";
  DWORD written = 0;
  ::WriteFile(h, body, static_cast<DWORD>(std::strlen(body)), &written, nullptr);
  ::CloseHandle(h);
}

const std::wstring& Tr(Str id) {
  LoadLanguage();
  return Table(id, g_lang);
}

const wchar_t* TrW(Str id) {
  return Tr(id).c_str();
}

}  // namespace i18n
}  // namespace ime
