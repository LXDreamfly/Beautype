# 配置说明

> Beautype 的配置均为本地文件，位于用户目录；无云端依赖、无遥测。

## 配置总览

| 文件 | 位置 | 内容 |
|---|---|---|
| `theme.yaml` | `%APPDATA%\Beautype\config\` | 皮肤主题（颜色 / 字体 / 布局 / 皮肤模式） |
| `update.yaml` | `%APPDATA%\Beautype\config\` | 自动更新源（可覆盖默认 GitHub 源与镜像） |
| `language` | `%APPDATA%\Beautype\config\` | 界面语言：`zh`（默认）或 `en` |
| `pet_config.json` | `%LOCALAPPDATA%\rime_beautify\` | 桌宠状态（皮肤 / 静音 / 按键显示 / 场景 / 位置 / 缩放） |
| `user.yaml` 等 | `%APPDATA%\Beautype\user\` | Rime 用户配置与部署数据 |

## 1. theme.yaml（皮肤主题）

格式见 `docs/skin-format.md` §3。通过设置面板修改后**热更新候选窗**，无需重启。

## 2. update.yaml（更新源）

**加载优先级**：环境变量 `BEAUTYPE_UPDATE_URL` > `config\update.yaml` > 内置默认 GitHub Releases。

```yaml
mirror-prefix: "https://mirror.ghproxy.com/"
```

- **版本源**：默认 `https://api.github.com/repos/LXDreamfly/Beautype/releases/latest`（仅正式版主线，跳过 prerelease），可用环境变量或配置文件覆盖
- **`mirror-prefix`**：下载加速镜像前缀，可追加 / 覆盖内置镜像（内置 ghproxy 等）；直连 GitHub 下载失败时按序自动尝试镜像
- 下载后执行 **SHA256 校验**，与 Release 公告值不符则丢弃并切换镜像重试

## 3. language（界面语言）

纯文本配置，内容仅允许 `zh` 或 `en`，缺省为 `zh`。在语言栏右键菜单或服务托盘菜单的 **Interface Language / 界面语言** 中切换后立即写入；当前进程即时刷新，其他已打开窗口下次打开时读取新语言。

## 4. pet_config.json（桌宠状态）

记录桌宠运行状态，重启自动恢复：

- 当前皮肤、静音、按键显示（HUD）开关、当前场景
- 窗口位置与缩放（200–800px）

由桌宠右键菜单 / 左键拖拽 / 滚轮缩放修改时自动保存。

## 5. Rime 用户配置（user.yaml 等）

`%APPDATA%\Beautype\user\` 为 Rime 用户目录：方案、用户词典、部署产物（build / rime_ice.userdb / log）。由 BeautypeDeployer 管理部署。
