# Beautype 架构说明

> Beautype（敲可爱）= **Rime 输入法前端 + Rive 桌宠**，一体化 Windows 桌面工具。
> 客户端-服务端架构：轻量 TSF 前端 ↔ 常驻服务进程（命名管道 IPC）↔ librime 引擎。
> 版本：v1.0（2026-10-23）

## 1. 进程与模块

| 组件 | 类型 | 职责 |
|---|---|---|
| **BeautypeServer.exe** | 服务进程（常驻，注册表自启） | librime 引擎；命名管道 IPC 服务端；候选窗渲染；托盘；桌宠事件推送；自动更新 |
| **beautype_tsf.dll** | TSF 文本服务（x64/x86） | 按键捕获与转发；组合串/候选/上屏；语言栏图标与右键菜单；吸管取色 |
| **pet_preview.exe** | 桌宠进程 | Rive 渲染（透明置顶分层窗口，D3D11）；皮肤库；右键菜单；全局键盘钩子（HUD/isTyping 兜底） |
| **BeautypeDeployer.exe** | 部署工具 | Rime 数据部署；设置面板（独立进程，避免服务进程阻塞） |
| **register_tip.exe** | 注册工具 | TSF 注册 / 注销 |

组件关系：

```
BeautypeSetup.exe（NSIS 安装器）
 ├─ BeautypeServer.exe      常驻服务进程
 ├─ beautype_tsf.dll        输入法前端（x64 + x86）
 ├─ BeautypeDeployer.exe    Rime 部署 / 设置面板
 ├─ register_tip.exe        TSF 注册
 ├─ pet\pet_preview.exe     桌宠
 ├─ pet\beautype_pet.riv    默认皮肤
 └─ data\rime\              词典 / 方案 / lua
```

## 2. 核心数据流

### 打字链路

```
键盘 → beautype_tsf.dll（TSF KeyDown）
     → 命名管道 IPC（JSON：vk/mask）→ BeautypeServer（librime 引擎）
     → 候选快照（JSON：input/list/highlight）→ 服务进程候选窗渲染
     → 选择/上屏 → CommitText → TSF 插入文档
```

### 桌宠链路

```
BeautypeServer（PetPushServer，本地 TCP + token 鉴权）→ pet_preview
  ├ isTyping 打字脉冲
  └ 语言栏/托盘「打开桌宠」→ 拉起 pet_preview.exe <riv>
pet_preview 无输入法时：全局低级键盘钩子驱动 HUD / isTyping（兜底）
```

### 更新链路

```
语言栏「检查更新」→ BeautypeServer（updater）
  → GitHub Releases API（/releases/latest，仅正式版主线）
  → 下载安装包 → SHA256 校验（失败自动丢弃并切换国内镜像兜底）→ 运行安装器
```

## 3. IPC 协议

- 传输：命名管道 `\\.\pipe\beautype.ime`（`ime_protocol.h` 的 `kImePipeName`）
- **帧格式固定（写死）**：4 字节大端长度前缀 + UTF-8 JSON 载荷（`shared/ipc/frame.h`）
- **消息类型枚举**：`ime_protocol.h` 定义 `MsgType`（Hello / KeyDown / SelectCandidate / SetOption / UpdatePosition / Candidates / Commit / Status / Ack / Ping / Pong 等）
- 载荷为极简 JSON，字段固定，不引入第三方 JSON 依赖
- 桌宠 TCP 推送复用同一协议（pet_push_server）
- 服务端基于移植的 WeaselIPC 库实现；**管道名与消息协议为 Beautype 自研定义**

## 4. 关键设计决策

- **候选窗在服务进程内渲染**：引擎与 UI 同进程，避免跨进程高频快照传输的延迟
- **皮肤与功能链路解耦**：候选/上屏/翻页/选词与皮肤模式（角色图 / 输入框图 / 默认模式）无关，只换 UI 样式
- **桌宠输入解耦**：isTyping 动画由打字驱动；「按键显示」仅控制 HUD 显隐
- **位置上报会话级去重**：同会话内相同光标位置去重，会话切换 / 失焦重连时强制重报（对齐官方 TSF 语义，同时避免自研皮肤反复重绘卡顿）
- **吸管取色**：取色期间捕获全局鼠标，移出窗口仍保持取色指针，点击采样后恢复
- **深色 / 浅色自适应**：候选窗、取色窗口跟随父级配色方案
- **自动更新**：GitHub Releases 主源 + 国内镜像兜底 + SHA256 校验（下载损坏自动丢弃重试）
- **语言栏右键菜单**：中英切换 / 自定义皮肤 / 词典管理 / 用户数据同步 / 打开桌宠 / 置顶桌宠 / 检查更新 / 退出等

## 5. 用户数据

| 项 | 位置 |
|---|---|
| 服务 / TSF 日志 | `%APPDATA%\Beautype\` |
| rime 用户目录 | `%APPDATA%\Beautype\user\` |
| 桌宠用户数据 | `%LOCALAPPDATA%\rime_beautify\` |

配置项详见 `docs/config-schema.md`；构建与打包详见 `docs/dev-guide.md`。
