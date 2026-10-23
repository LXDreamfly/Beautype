# 第三方组件与许可

| 组件 | 用途 | 许可 | 集成方式 |
|---|---|---|---|
| [librime](https://github.com/rime/librime) | Rime 输入引擎（引擎层） | BSD-3-Clause | git submodule（`third_party/librime`） |
| [rive-runtime](https://github.com/rive-app/rive-runtime) | Rive 骨骼动画运行时（C++） | MIT | git submodule（`third_party/rive-runtime`） |

## 义务说明

- **librime（BSD-3）**：保留版权与许可声明；本项目若修改 librime 源码，修改部分沿用 BSD-3。
- **rive-runtime（MIT）**：保留版权与许可声明。
- **模型素材**：示例模型一律附带各自许可；用户自行上传的模型权利归用户所有，本项目不代为主张。

## 新增第三方组件检查清单

1. 确认许可与 GPL-3.0 兼容或可隔离（独立进程 / 独立链接）；
2. 在 `docs/licenses.md` 与本文档登记用途、许可、集成方式；
3. 专有依赖一律走下载脚本，禁止提交二进制；
4. 示例素材标明作者与许可，禁止无许可素材入库。
