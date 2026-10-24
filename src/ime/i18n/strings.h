// Beautype 运行时界面语言表（方案 A：单安装包 + 运行时切换，中文/英文）。
//
// 边界：
//  - 本模块只覆盖用户可见宽字符串；源码注释、日志、候选词本身不进入语言表。
//  - 系统输入法注册剖面名保持固定品牌名，不随本配置切换。
//  - 语言配置存 %APPDATA%\Beautype\config\language，纯文本 zh / en，缺省 zh；
//    各进程启动时读取，当前进程切换后立即刷新内存态，其他进程下次打开窗口时读取。
#pragma once

#include <string>

namespace ime {
namespace i18n {

enum class Lang { Zh, En };

// 运行时可见文案 ID。新增可见文案先在此登记，再在 strings.cpp 补中英文。
enum class Str : int {
  kBrandDescription = 0,
  kCompositionDescription,
  kCandidateListDescription,

  kLanguageMenuTitle,
  kLanguageChinese,
  kLanguageEnglish,

  kToggleToChinese,
  kToggleToEnglish,
  kMenuSettings,
  kMenuDictionary,
  kMenuSync,
  kMenuOpenPet,
  kMenuPinPet,
  kMenuUserDir,
  kMenuLogDir,
  kMenuDeploy,
  kMenuCheckUpdate,
  kMenuExit,
  kMenuSchema,
  kMenuAsciiToggle,

  kTooltipEnglish,
  kTooltipChinese,

  kSettingsTitle,
  kSettingsSkinTitle,
  kPresetSection,
  kColorSection,
  kColorBackground,
  kColorHighlightText,
  kColorText,
  kColorHighlightBackground,
  kFontSection,
  kSkinModeSection,
  kRoleHint1,
  kRoleHint2,
  kFrameHint1,
  kPreview,
  kFrameAlignHint,
  kAxisX,
  kAxisY,
  kAxisWidth,
  kAxisHeight,
  kBackgroundNone,
  kBackgroundLabel,
  kFileFilterPng,

  kErrorCreateConfigDir,
  kErrorCopyBackground,
  kErrorCopyImage,
  kErrorImageLocked,
  kErrorDecodeImage,
  kErrorRoleSize,
  kErrorFrameSize,
  kErrorWriteTheme,
  kErrorPresetName,
  kErrorCreatePresetDir,
  kErrorSavePreset,
  kErrorSelectPreset,
  kErrorLoadPreset,
  kErrorDeletePreset,

  kBtnLoad,
  kBtnDelete,
  kBtnSavePreset,
  kBtnPickFont,
  kRadioRoleImage,
  kRadioFrameImage,
  kBtnPickImage,
  kBtnOpenFolder,
  kBtnReset,
  kBtnSaveApply,
  kBtnClose,

  kColorPickerTitle,
  kBtnPickScreenColor,
  kBtnOk,
  kBtnCancel,
  kPickingHint,

  kUpdateTitle,
  kUpdateFoundPrefix,
  kUpdateFoundSuffix,
  kUpdateReadyPrefix,
  kUpdateReadySuffix,
  kUpdateFail,
  kUpdateDone,

  kCount
};

// 当前进程缓存的语言；首次调用时从配置文件读取，缺省中文。
Lang CurrentLanguage();
bool IsEnglish();

// 强制重新读取配置文件；其他进程下次构建菜单/窗口前调用，读取另一进程刚写入的语言。
void ReloadLanguage();

// 切换当前进程语言并写配置文件；persist=false 仅切换内存态（测试/特殊场景用）。
void SetLanguage(Lang lang, bool persist = true);

// 取当前语言文案；未知 ID 返回空宽字符串。
const std::wstring& Tr(Str id);
const wchar_t* TrW(Str id);

}  // namespace i18n
}  // namespace ime
