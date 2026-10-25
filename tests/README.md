# 测试

- `unit/` — C++ 单元测试（无第三方框架，断言失败返回非零）。当前覆盖 IPC 帧编解码与协议常量/端口文件序列化。
- `integration/`（M2 起）— 跨进程集成测试：Socket 重连、三模式切换、窗口 flag 断言、端到端 key_event 冒烟。
- `assets/` — 测试夹具（示例 .riv / 皮肤 PNG / model3.json 等）。

原则：每个测试对应一个可被推翻的错误机制，见 `docs/architecture.md` 测试策略节。
