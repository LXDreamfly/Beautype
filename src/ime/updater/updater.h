#pragma once
// Beautype 自动更新模块（v1.0）：
//  检查更新（语言栏/托盘菜单 → 主源 GitHub Releases API + 可配置镜像兜底）
//  → 有新版弹窗确认 → 下载（URLDownloadToFile）→ 完成弹窗 → 运行安装器。
// 设计对齐：官方 Windows 应用常见更新器（GitHub Releases 为发布源，
// 语义化版本比较，只跟正式主线，不碰 prerelease）。
#include <Windows.h>

namespace ime::updater {

// 更新流程消息（发给 owner 窗口，owner 在 WndProc 里转发给 HandleUpdateMessage）
constexpr UINT kMsgUpdateFound = WM_APP + 201;    // wParam = 0（发现新版，弹窗确认）
constexpr UINT kMsgUpdateReady = WM_APP + 202;    // 下载完成（弹窗：是否立即安装）
constexpr UINT kMsgUpdateFail = WM_APP + 203;     // 检查/下载失败（弹窗提示）
constexpr UINT kMsgUpdateDone = WM_APP + 204;     // 已是最新 / 用户取消（无操作）

// 启动一次检查更新（内部起后台线程；有新版则经 kMsgUpdateFound 回调 owner）
void StartCheckUpdate(HWND owner);

// owner 窗口过程里统一转发：case WM_APP+201..204 → HandleUpdateMessage
void HandleUpdateMessage(HWND owner, UINT msg);

}  // namespace ime::updater
