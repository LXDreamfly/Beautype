# IPC 协议说明

> Beautype 的本地 IPC 分两套：**命名管道**（TSF DLL ↔ BeautypeServer，输入法主链路）与**本地 TCP**（BeautypeServer ↔ 桌宠，打字动画推送）。

## 1. 命名管道协议（TSF ↔ Server）

### 传输

- 管道名：`\\.\pipe\beautype.ime`（`src/shared/ipc/ime_protocol.h` 的 `kImePipeName`）
- 帧格式：**4 字节大端长度前缀 + UTF-8 JSON 载荷**（`src/shared/ipc/frame.h`；单帧上限 1 MiB）
- 载荷为极简 JSON，字段固定，不引入第三方 JSON 依赖

### 消息类型（enum class ImeMsg）

| 值 | 消息 | 方向 | 说明 |
|---|---|---|---|
| 1 | Hello | DLL→Server | 连接握手（ver） |
| 2 | HelloAck | Server→DLL | 握手应答（ok） |
| 3 | KeyDown | DLL→Server | 按键（vk, mask） |
| 4 | SelectCandidate | DLL→Server | 选择候选（i） |
| 5 | SetOption | DLL→Server | 设置项（k, v） |
| 6 | UpdatePosition | DLL→Server | 候选窗屏幕位置（GetTextExt 结果） |
| 7 | Candidates | Server→DLL | 候选 / 上下文快照（input/list/highlight） |
| 8 | CommitText | Server→DLL | 上屏文本 |
| 9 | Status | Server→DLL | 状态推送（ascii_mode / schema） |
| 10 | Ack | Server→DLL | 动作应答 |
| 11 / 12 | Ping / Pong | 双向 | 保活 |

## 2. 桌宠 TCP 协议（Server ↔ Pet）

### 传输与握手

- 传输：TCP loopback（127.0.0.1），仅本机通信
- 端口发现：IME 启动时监听随机端口，将 `{"protocol_version":1,"port":<port>,"token":"<hex>"}` 写入 `%LOCALAPPDATA%\rime_beautify\runtime\ime.port.json`；桌宠读取该文件后连接
- 鉴权：桌宠连接后先发 `hello`（携带 token）；校验失败立即断开，防止本地其他进程伪造按键事件
- 重连：IME 重启会重写端口文件；桌宠检测断线后重读端口文件并退避重连（1s→2s→4s→…上限 30s）
- 保活：双方周期发送 `ping`，超时（默认 10s 未收 `pong`）判定断线

### 消息

| type | 方向 | 载荷 | 说明 |
|---|---|---|---|
| 1 hello | 桌宠→IME | `token` | 连接鉴权 |
| 2 hello_ack | IME→桌宠 | `protocol_version`, `ime_ready` | 接受连接；版本不符则拒绝 |
| 3 key_event | IME→桌宠 | `vk`, `scan`, `make`, `ts` | 按键按下 / 抬起 |
| 4/5 ping/pong | 双向 | — | 保活 |
| 6 bye | 双向 | — | 正常关闭；桌宠右键退出时发（IME 记住"用户主动退出"，本次服务生命周期不再自动拉起） |
| 16 commit_text | 预留 | `text` | 上屏文本（二期） |
| 17 mouse_event | 预留 | `x`, `y` | 鼠标事件（二期） |
| 18 config_sync | 预留 | — | 配置同步（二期） |

> **key_event 语义**：IME 在 TSF 键盘事件回调中收到按键时推送（输入法激活状态）。事件为 **best-effort**：丢包仅导致动画错过一拍，无状态损坏；按键为幂等触发器，重连后重复事件无副作用。
>
> **废弃**：~~pet_visibility~~（2026-10-14）：用户定稿"切走输入法不隐藏桌宠"，桌宠常驻、退出靠右键菜单。

## 3. 版本协商与失败语义

### 版本协商

- 不兼容变更（帧格式 / 消息结构 / 类型语义）必须递增 `kProtocolVersion`
- 对端 `v` 不一致：拒绝连接并返回明确错误提示（升级提示），**不静默降级**
- 新增消息类型视为向后兼容扩展，可跨版本发送（未知类型应忽略并记录）

### 失败语义

| 场景 | 行为 | 恢复 |
|---|---|---|
| 事件丢失 | 动画错过一拍（可接受） | 无需恢复 |
| 重复 / 并发（重连） | 按键事件幂等 | 无需去重 |
| IME 崩溃 | 连接断开，桌宠按键动画暂停并显示状态 | 桌宠重连（重读端口文件）；**不自动切回全局钩子**（不超出用户所选模式） |
| 版本不符 | 拒绝连接 + 提示升级 | 用户升级任一组件 |
| 端口文件损坏 / 过期 | 桌宠重读失败则进入待机 | 等待 IME 重写端口文件 |
