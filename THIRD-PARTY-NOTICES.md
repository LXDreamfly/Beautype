# 第三方组件与许可（Third-Party Notices）

本文件逐项列出 Beautype 分发与构建涉及的第三方组件：许可、官方来源、版本/commit、本项目使用方式、是否修改及改动文件清单。许可证全文以各上游仓库为准（SPDX 标识见 [SPDX License List](https://spdx.org/licenses/)）。与仓库根 LICENSE（GPL-3.0）的兼容性结论见文末。

## 一、编译进二进制的第三方库

| 组件 | 用途 | 许可（SPDX） | 官方来源 | 版本 / commit | 集成方式 | 是否修改 |
|---|---|---|---|---|---|---|
| librime | Rime 输入引擎（静态链接 `rime-static`） | BSD-3-Clause | https://github.com/rime/librime | `7bc3fb0005a03aff1c086611e4d52bb7f6444fdd` | 不随仓库分发；`scripts/get-librime.cmd` 克隆并固定 commit；根 CMake 以 `-DLIBRIME_SOURCE_DIR` 引入 | 未改源码（构建期经 CMake 缓存变量指定 vcpkg 静态依赖路径） |
| weasel（WeaselIPC / WeaselIPCServer） | 服务化 IPC：TSF DLL ⇄ 服务进程管道通信 | GPL-3.0 | https://github.com/rime/weasel | `d73f6295e8252ed2f7b9c12bae32e9001b1afdaa` | 不随仓库分发；`scripts/apply-weasel-patch.cmd` 克隆、固定 commit、应用本地补丁；`src/ime/CMakeLists.txt` 直接编译其 12 个源文件（见下「weasel 派生说明」） | 是（本地补丁，见下） |
| rive-runtime | Rive 骨骼动画运行时（桌宠 D3D11 PLS 渲染） | MIT | https://github.com/rive-app/rive-runtime | `5551696a826ff546775fffe46ca78824b1f1dc51`（tag `runtime-v0.1.582` 后 1 个提交） | git submodule（`third_party/rive-runtime`）+ 本地构建配置补丁 `patches/rive-runtime-patches/premake-build-config.patch` | 是（仅 premake 构建配置，6 个文件，见下） |
| Boost（wserialization 等） | WeaselIPC 序列化依赖（纯头 + b2 静态库） | BSL-1.0 | https://www.boost.org | 1.87.0 | 头文件 `C:/boost_inc` + b2 静态库 `stage/lib`（x86：`stage-x86/lib`，`-DBOOST_STAGE_LIB` 可覆盖） | 否 |
| yaml-cpp | 皮肤 `theme.yaml` 解析 | MIT | https://github.com/jbeder/yaml-cpp | vcpkg 安装版 | `find_package(yaml-cpp CONFIG REQUIRED)` | 否 |
| glog | librime 内部日志 | BSD-3-Clause | https://github.com/google/glog | vcpkg 安装版 | 静态链接 | 否 |
| gflags | glog 依赖 | BSD-3-Clause | https://github.com/gflags/gflags | vcpkg 安装版 | 静态链接 | 否 |
| leveldb | librime 用户词典存储 | BSD-3-Clause | https://github.com/google/leveldb | vcpkg 安装版 | 静态链接 | 否 |
| marisa-trie | librime 词典检索 | BSD-2-Clause OR LGPL-2.1-or-later | https://github.com/google/marisa-trie | vcpkg 安装版 | 静态链接（经 librime） | 否 |
| OpenCC | 简繁转换 | Apache-2.0 | https://github.com/BYVoid/OpenCC | vcpkg 安装版 | 静态链接（经 librime） | 否 |
| Windows SDK（msctf / gdiplus / shell32 / winhttp 等） | 系统 API | 微软专有（随 VS 安装） | https://developer.microsoft.com/windows/downloads | — | 链接系统库 | 否 |

### rive-runtime 桌宠构建期依赖（随 pet_preview.exe 打包进二进制）

经 rive-runtime 上游 premake（`tests/dependencies` 各 fork/镜像仓库）拉取并静态链接，许可以各上游仓库 LICENSE 为准（MIT / BSD / zlib 系列）：harfbuzz、libpng、libjpeg、libwebp、zlib、SheenBidi、yoga、luau（脚本 VM）、miniaudio（音频）。本仓库不包含这些依赖源码；构建 rive-runtime 时经其官方依赖机制获取。

**rive-runtime 本地改动（补丁 `patches/rive-runtime-patches/premake-build-config.patch`，共 6 个文件）**：`.gitignore`、`cg_renderer/premake5.lua`、`decoders/premake5_v2.lua`、`premake5_v2.lua`、`renderer/premake5_pls_renderer.lua`、`tests/rive_tools_project.lua`。差异摘要：编译器告警开关（如 `fatalwarnings`）仅对非 MSVC 工具集生效，避免 MSVC 编译被上游默认告警策略中断；`.gitignore` 补充本地备份目录说明。可复现性已核验：在 5551696 干净 checkout 上 `git apply` 应用后，6 个文件内容与开发仓本地提交 `ea9ad5e` 逐字节一致（Windows 若因行尾报 `patch does not apply`，加 `--ignore-whitespace` 重试）。

**开发期测试依赖锁定（仅供追溯，未入公开补丁）**：开发仓提交 ea9ad5e 另以 gitlink 锁定了 11 个测试依赖 commit —— `Tehreer_SheenBidi_v2.6 @ adfccc46`、`dabeaz_ply_3.11 @ 0f398b72`、`glennrp_libpng_libpng16 @ 33d01504`、`glfw_glfw_3.4 @ 7b6aead9`、`madler_zlib_v1.3.1 @ 51b7f2ab`、`microsoft_DirectX-Headers_v1.615.0 @ 27d63115`、`rive-app_harfbuzz_rive_13.1.1 @ 08d34675`、`rive-app_libjpeg_v9f @ 2f995b50`、`rive-app_miniaudio_rive_changes_5 @ 3a8b070f`、`rive-app_yoga_rive_yoga_fb91f6cb0f8f @ 9a6ac7fa`、`webmproject_libwebp_v1.4.0 @ 845d5476`。公开补丁不含 gitlink 条目（`git apply` 无法写入 gitlink）；这些依赖由 rive 官方 premake 自动拉取，如需复现开发期精确版本可手动 checkout 上述 commit。

## 二、仅打包 / 构建期工具（不入二进制）

| 组件 | 用途 | 许可 | 来源 |
|---|---|---|---|
| NSIS | 安装器生成（makensis） | zlib | https://nsis.sourceforge.io |
| CMake | 构建系统 | BSD-3-Clause | https://cmake.org |
| Visual Studio Build Tools（MSVC） | 编译器 | 微软专有 | https://visualstudio.microsoft.com |

## 三、weasel（GPL-3.0）派生说明 —— 对等源码义务

Beautype 服务进程基于移植的 WeaselIPC 库实现（见 `docs/architecture.md`：服务端基于移植的 WeaselIPC 库实现，管道名与消息协议为 Beautype 自研定义），属 weasel（GPL-3.0）派生作品；项目整体以 GPL-3.0 发布（根 `LICENSE`）。

**直接编译的 weasel 上游源文件（12 个，来自 rime/weasel @ d73f6295，编译进 `rime_ipc` / `rime_ipc_server`）**：

- `WeaselIPC/ActionLoader.cpp`
- `WeaselIPC/Committer.cpp`
- `WeaselIPC/Configurator.cpp`
- `WeaselIPC/ContextUpdater.cpp`
- `WeaselIPC/Deserializer.cpp`
- `WeaselIPC/PipeChannel.cpp`
- `WeaselIPC/ResponseParser.cpp`
- `WeaselIPC/Styler.cpp`
- `WeaselIPC/WeaselClientImpl.cpp`
- `RimeWithWeasel/WeaselUtility.cpp`
- `WeaselIPCServer/SecurityAttribute.cpp`
- `WeaselIPCServer/WeaselServerImpl.cpp`

**本地修改的上游文件（5 个，差异见 `patches/weasel-ime-activated.patch`）**：

- `WeaselIPC/WeaselClientImpl.cpp`
- `WeaselIPC/WeaselClientImpl.h`
- `WeaselIPCServer/WeaselServerImpl.cpp`
- `WeaselIPCServer/WeaselServerImpl.h`
- `include/WeaselIPC.h`

改动内容：新增 `ImeActivated(bool)` 接口（TSF 激活/停用通知 → 桌宠显隐），协议新增 `WEASEL_IPC_IME_ACTIVATED` 消息（补丁 24 行新增，SHA256 `C1E0EAB8BFC4B117DFE6DCDC18E8F8249A6E0AA41D7FAE597993C0352C152923`）。

**对等源码获取**：本仓库不含 weasel 整树；`scripts/apply-weasel-patch.cmd` 从官方仓库克隆 commit d73f6295 并应用上述补丁，即得到与发布二进制对应的完整 weasel 源码与改动。Beautype 自研前端源码（`src/ime/tsf/*`、`src/ime/ui/*`、`src/ime/skin/*`、`src/ime/engine/*` 等）为本项目自有代码，随仓库 GPL-3.0 发布。

## 四、仓库内数据与素材

| 数据 | 来源 | 许可 |
|---|---|---|
| `data/rime/` 内置输入方案数据 | librime `data/minimal`；rime/rime-wubi；rime/rime-double-pinyin；rime-ice 预设 | 混合：BSD-3-Clause（librime 数据）、LGPL-3.0（rime-wubi、rime-double-pinyin）、GPL-3.0（rime-ice 预设）；详见 `data/rime/README.md` 授权说明 |
| `assets/brand/`、`assets/screenshots/` | 本项目自绘 | 随项目 GPL-3.0 |
| `packaging/Beautype/beautype-logo.ico`、`src/**/*.ico` | 本项目自绘 | 随项目 GPL-3.0 |
| `packaging/Beautype/pet/beautype_pet.riv` | 本项目默认示例模型 | 随项目 GPL-3.0；用户自行上传的模型权利归用户所有，本项目不代为主张 |

## 五、与 GPL-3.0 的兼容性结论

- BSD-3-Clause / BSD-2-Clause / MIT / BSL-1.0 / zlib：宽松许可，可与 GPL-3.0 组合分发；
- LGPL-3.0 / LGPL-2.1-or-later（marisa 备选、rime-wubi、rime-double-pinyin）：与 GPL-3.0 兼容，组合作品可按 GPL-3.0 发布；
- Apache-2.0：GPL-3.0 兼容（GPLv3 允许与 Apache-2.0 组件组合）；
- GPL-3.0（weasel、rime-ice）：同许可组合；weasel 派生义务见第三节；
- 微软 SDK 库：随 Visual Studio 许可使用，不改变本项目的 GPL-3.0 发布。

## 六、新增第三方组件检查清单

1. 确认许可与 GPL-3.0 兼容或可隔离（独立进程 / 独立链接）；
2. 在本文档登记用途、许可、来源版本与集成方式；
3. 专有 / 大体积依赖一律走获取脚本，禁止提交二进制；
4. 示例素材标明作者与许可，禁止无许可素材入库。
