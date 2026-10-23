// 可视化皮肤设置面板（M1.2）：候选窗颜色/字体/PNG 背景图/布局 全部
// 在界面里点选，无需手改 theme.yaml。
//
// 官方参考（微软 Learn「使用常用对话框」）：
//   - 选颜色：CHOOSECOLOR + ChooseColor（CC_RGBINIT 预选当前色，CC_FULLOPEN 展开）
//   - 选字体：CHOOSEFONT + ChooseFont（CF_SCREENFONTS|CF_INITTOLOGFONTSTRUCT 预填）
//   - 选文件：OPENFILENAME + GetOpenFileName（OFN_PATHMUSTEXIST|OFN_FILEMUSTEXIST）
// 这三个通用对话框是 Win32 标准能力，链接 comdlg32.lib。
//
// 服务化后：设置面板在服务进程内运行（托盘「设置」打开）。热更新不再依赖
// TSF DLL 的 RimeTextService，改为 on_apply 回调（调用方负责写盘 + 通知
// 候选窗线程换肤）。回调在设置面板线程执行，调用方须自行做线程切换。
#pragma once

#include <functional>
#include <string>

#include <windows.h>

#include "skin/theme.h"

namespace ime {
namespace ui {

// 模态设置面板。owner：父窗口；config_dir：theme.yaml 所在目录
// （%APPDATA%\Beautype\config）；on_apply：用户点「保存并应用」时回调
// （参数为编辑后的完整主题）。
// 返回 true = 用户点击「保存并应用」；false = 关闭/取消。
bool RunSettingsDialog(HWND owner, const std::string& config_dir,
                       std::function<void(const ime::skin::Theme&)> on_apply);

}  // namespace ui
}  // namespace ime
