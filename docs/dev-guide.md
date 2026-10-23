# 开发指南

## 1. 环境要求

- Windows 10 / 11 x64
- Visual Studio 2022（**使用 C++ 的桌面开发**工作负载，含 MSVC 与 Windows SDK）
- CMake ≥ 3.20（VS2022 BuildTools 自带）
- NSIS 3（打包安装器）
- Git（Windows）

## 2. 获取代码

```powershell
git clone --recurse-submodules https://github.com/LXDreamfly/Beautype.git
# 若已克隆未带子模块：
git submodule update --init --recursive
```

- 子模块：`third_party/librime`（Rime 引擎）、`third_party/rive-runtime`（Rive 运行时）
- 依赖：boost（vcpkg / 本机头文件）、rime-ice（词典源，`data/rime` 已裁剪）

## 3. 构建

### 输入法（CMake，VS2022）

```powershell
# x64（System32 / 64 位宿主：Chrome、豆包、explorer 等）
cmake --build build    --config Release --target rime_tsf
cmake --build build    --config Release --target rime_server

# x86（SysWOW64 / 32 位宿主：WPS 等）
cmake --build build-x86 --config Release --target rime_tsf
```

> **双架构必须都编（发版硬性流程）**：TSF DLL 按进程位数加载——64 位进程加载 `System32\beautype_tsf.dll`，32 位进程加载 `SysWOW64\beautype_tsf.dll`。install.nsi 从 `packaging\Beautype\x86\beautype_tsf.dll` 打包 x86 份。**只编 x64 树 → x86 包内 DLL 是旧代码** → 一切"只在 WPS 复现"的缺陷（语言栏右键、owner、ImeActivated）其实都是 x86 DLL 陈旧，不是宿主兼容问题。

### 桌宠

```powershell
tools\smoke\compile_pet.bat   # vcvars64 + cl，链接 rive 静态库
```

### 发版核对清单

1. `build\src\ime\Release\beautype_tsf.dll` 与 `build-x86\src\ime\Release\beautype_tsf.dll` 时间戳差 < 1 小时；
2. 新 x86 DLL 已拷到 `packaging\Beautype\x86\beautype_tsf.dll`；
3. 安装后抽查：System32 与 SysWOW64 两份 DLL 时间戳一致。

## 4. 打包

```powershell
makensis packaging\Beautype\install.nsi → dist\BeautypeSetup.exe
```

NSIS 安装器：安装全部组件（输入法 + 桌宠）、卸载全部清除（含注册表、自启动、开始菜单、用户数据按需保留）。

## 5. 目录职责

| 目录 | 职责 |
|---|---|
| `src/shared/ipc/` | 协议帧编解码 + 消息定义（frame.h / ime_protocol.h） |
| `src/ime/` | 输入法（server / tsf / ui / skin / updater / deploy） |
| `src/ime/updater/` | 自动更新（GitHub Releases + 镜像兜底 + SHA256 校验） |
| `tools/` | 桌宠编译脚本等 |
| `docs/` | 文档（架构 / 协议 / 配置 / 皮肤格式） |

## 6. 提交约定

- 消息前缀：`build:` `feat:` `fix:` `docs:` `test:` `refactor:`
- 改动协议必须同步 `docs/ipc-protocol.md` 并递增版本
- 新增第三方依赖前阅读 `docs/licenses.md` 检查清单
