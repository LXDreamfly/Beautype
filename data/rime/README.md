# data/rime —— 敲可愛 Beautype 内置 Rime 数据

本目录是 Beautype 输入法的**自包含 Rime 数据**（M1 起取代对 librime
`data/minimal` 的依赖）。运行/测试时的 `RIME_SHARED_DATA_DIR` 指向本目录即可。

## 仓库内文件（小配置文件，随仓库维护）

| 文件 | 来源 | 说明 |
|---|---|---|
| `default.yaml` | 基于 librime `data/minimal/default.yaml` 定制 | 注册 3 方案；默认上下键翻页、左右键选词（key_binder）；标点表与官方一致 |
| `luna_pinyin.schema.yaml` | 基于 librime `data/minimal/luna_pinyin.schema.yaml` 定制 | 默认简体（simplification reset:1）；移除倉頡旁路分支 |
| `double_pinyin.schema.yaml` | rime/rime-double-pinyin 官方方案（定制：去 stroke 反查依赖、默认简体） | 自然碼雙拼，复用 luna_pinyin 词典 |
| `wubi86.schema.yaml` | rime/rime-wubi 官方方案（定制：去 pinyin_simp 反查依赖） | 五笔86 |
| `README.md` | 本文件 | 来源与授权说明 |

## CI 组装的大词典文件（不在仓库，CI 从上游拉取后与本目录合并）

| 文件 | 上游来源 |
|---|---|
| `luna_pinyin.dict.yaml` | librime `data/minimal`（BSD-3） |
| `essay.txt` | librime `data/minimal`（BSD-3） |
| `symbols.yaml` | librime `data/minimal`（BSD-3） |
| `wubi86.dict.yaml` | rime/rime-wubi 仓库根（LGPL-3.0，约 2.3MB） |

本地开发：需要完整数据时手动从上述上游拷贝这 4 个文件到本目录；
`ci.yml` 的「Assemble Rime data」步骤自动完成该组装，产物 zip 内 `data\rime` 为完整数据。

## 授权说明

- librime（含其 `data/minimal` 预置数据）：BSD-3-Clause
  （https://github.com/rime/librime/blob/master/LICENSE）
- rime/rime-double-pinyin：LGPL-3.0
  （https://github.com/rime/rime-double-pinyin/blob/master/LICENSE）
- rime/rime-wubi（含 wubi86 码表）：LGPL-3.0
  （https://github.com/rime/rime-wubi/blob/master/LICENSE）

自行定制的字段（key_binder 默认键位、simplification 默认值、scheme 精简）随
Beautype 本体开源协议发布。分发完整数据目录时请保留上游 LICENSE 声明（见各仓库链接）。
