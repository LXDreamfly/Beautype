# Beautype 错误与教训复盘（M1）

> 规则：每个踩过的错误必须追加（现象 → 根因 → 防再犯规则），动手前先读本清单。

---

## 2026-10-27 — std::filesystem::path(窄string) 构造器按 GBK 解释 UTF-8 字节 → 含中文预设名加载即闪退

### 现象
用户保存中文命名预设「bt兔.yaml」后，点"加载"窗口卡死、进程 0xc0000409（STATUS_STACK_BUFFER_OVERRUN / fastfail(7)）退出，事件日志 fault offset 恒为 0x1d2995（= `int 29h`，即 std::terminate → __fastfail(FAST_FAIL_FATAL_APP_EXIT=7)）。三次崩溃同偏移。英文预设「boo.yaml」全程正常。装 std::set_terminate 钩子后抓到未捕获异常消息：
`No mapping for the Unicode character exists in the target multi-byte code page.`（ERROR_NO_UNICODE_TRANSLATION 1113）。

### 根因
`theme.cpp ResolveImagePath()` 写 `std::filesystem::path(yaml_path).parent_path()`，其中 `yaml_path` 是 UTF-8 窄字符串（presets\bt兔.yaml）。
**MSVC 的 `path(const std::string&)` 构造器按系统 ANSI 代码页（本机 GBK/CP936）解释窄字节，而不是 UTF-8**。"兔"的 UTF-8 字节 `E5 85 94` 被 GBK 误读，碰到非法双字节对时宽→窄转换抛 `std::system_error`。
该异常在 `LoadThemeFromYaml` 的 try 块**外面**（try 只包了 YAML::Load），一路逃出 SettingsWndProc → DispatchMessageW → 主循环 → terminate → fastfail。
为什么 boo 不崩：文件名纯 ASCII，GBK 解释 ASCII 字节恒等。
为什么 10-24 原版 bt兔 不崩：原版 role_image 是**绝对路径**，`is_absolute()` 直接 return，根本不碰 parent_path()；用户重存后 role_image 变相对路径才走 parent_path 分支引爆。

### 防再犯规则
1. **凡可能含中文的窄 UTF-8 路径，一律用 `std::filesystem::u8path(utf8_string)` 构造 path；绝不用 `path(narrow_string)` 裸构造。** 宽 wstring 路径则直接 `path(wstring)`（UTF-16 直通，无代码页问题）。
2. 宽 wstring 禁止传给 `u8path`（u8path 要 UTF-8 字节串），编译报错；应直接 `path(wstring)`。
3. `LoadThemeFromYaml` 等解析函数的字段解析段必须整段包 try/catch(std::exception&)，落 LogF 后 return false，禁止异常逃出消息循环。
4. 新增"读文件→解析→出 path"路径时，逐个检查每个 `path(...)` 构造点用的是窄串还是宽串。

---

## 2026-10-27 — WM_ERASEBKGND 里 FillRect 裸画底色 → 双缓冲窗口拖动/吸管时闪

### 现象
10/23 版取色窗拖饱和度/亮度滑块、用吸管取色时 SV 色盘不闪；10/24 跨屏 DPI 修复后回归：快速拖动时色盘整区域闪一下纯底色。

### 根因
10/24 跨屏修复在 SettingsWndProc / ColorPickerProc 两处 WM_ERASEBKGND 里加了 `FillRect(...)` 直接画底色。滑块拖动每帧 `InvalidateRect(TRUE)` → 先触发 WM_ERASEBKGND 裸画到屏幕 DC（在双缓冲之外）→ 后 WM_PAINT 用内存 Bitmap 整帧 DrawImage 覆盖。快速拖动时两帧之间被屏幕采样，看到一帧纯底色。

### 防再犯规则
1. **已双缓冲的窗口，WM_ERASEBKGND 一律 `return 1`（不擦除），禁止 FillRect 裸画。** 跨屏 DPI 闪白靠类背景 `hbrBackground=nullptr`（系统不自动擦白）+ 过渡间隙露旧帧而非白帧 + WM_PAINT 整帧贴上解决，不靠 WM_ERASEBKGND 填底色。
2. 新增窗口类时 hbrBackground 直接设 nullptr；擦除职责全部交给 WM_PAINT 双缓冲。
3. 全屏取色遮罩等无背景窗口的 WM_ERASEBKGND 同样 return 1。

---

## 2026-10-27 — 设置窗口保存同名预设静默覆盖（无确认弹窗）

### 现象
用户改完内容后用与已有预设相同的名字点"保存"，无任何提示直接覆盖；用户预期"已有同名文件，是否覆盖？"弹窗。

### 根因
`SavePreset` 直接 `WriteThemeToYaml(...)`，存在性检查与用户确认缺失。

### 防再犯规则
写盘覆盖类操作（保存预设、导出配置、覆盖文件）必须先 `exists(path)`，存在则 MessageBoxW(MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2) 二次确认；选"否"直接 return。删除类操作同理。
---

## 2026-10-27 — 新注释里重写回 C:\Users 示例路径 → 公开仓同步时泄漏复发

### 现象
10/26 在 clean-tmp 提交 c5ba82b 清理了 theme.cpp 注释里的 C:\Users\... 示例文字；10/27 v1.1.1 新提交 add509211 在 theme.cpp 新增的根因注释里再次写回 `// C:\Users\... 里的 \U ...`，整文件同步到公开仓时泄漏复发（公开仓 tumen / C:\Users / C:/Users 全仓 grep 命中 1 处）。

### 根因
清理只发生在同步边界（clean-tmp），未回流到源码源头（开发仓）。开发仓 theme.cpp 注释自始带 C:\Users\...，10/27 写新根因注释时照旧抄了带路径的表述；两仓文件通过"整文件复制"同步，路径跟着回来。昨日 2c91f7b（launcher 路径）与 25cea00（install.nsi / compile_pet 相对化）在开发仓有对应提交 1b0b635dd，唯独 theme.cpp 注释清理只在 clean-tmp 一侧。

### 防再犯规则
1. 新写注释 / 文档禁止出现字面绝对路径示例（C:\Users、C:\dev、D:\ 等机器相关路径）；要举例一律写泛化描述（如"Windows 绝对路径"）或 \、${__FILEDIR__}、%~dp0 等相对写法。
2. 同步前三项检查（tumen、C:\Users、C:/Users grep）命中时，必须先清开发仓源头再同步，禁止只改 clean-tmp 一侧。
3. 公开仓 164 个文件内任何注释清理，必须同时落到开发仓对应文件，并用 git hash-object 核对两仓哈希一致。
---

## 2026-10-27 — WM_ERASEBKGND return 1（修拖动闪动）→ 窗口首帧黑白色块回归

### 现象
v1.1.1 把设置窗 / 取色窗 WM_ERASEBKGND 从"FillRect 填底色"改为 return 1（修对齐拖动 / SV 色盘拖动闪动）后，窗口刚打开瞬间黑一块白一块（露出屏幕旧内容）。此前 10/22 起"首帧防闪"靠 WM_SETREDRAW 冻结 + 解冻 + RedrawWindow + ShowWindow，但 ShowWindow 后系统才异步发 WM_PAINT，显示到首帧画完之间无任何东西擦背景。

### 根因
WM_ERASEBKGND return 1 + 类背景 hbrBackground=nullptr = 首帧绘制完成前窗口表面完全不初始化，谁都不擦 → 露旧帧 / 垃圾。10/24 的 FillRect 版本拖动时闪（v1.1.1 修的），但它恰好充当了首帧前的底色覆盖。两个需求（拖动不闪 / 首帧不露脏）不能靠 WM_ERASEBKGND 单条分支同时满足：防闪靠 return 1 + 双缓冲，首帧靠"不画好不显示"。

### 防再犯规则
1. WM_ERASEBKGND 保持 return 1（双缓冲标准）时，窗口显示前必须同步画完整帧：创建不带 WS_VISIBLE，解冻 + RedrawWindow(INVALIDATE) 后先 UpdateWindow(hwnd)（隐藏态画完整帧）再 ShowWindow——不画好不显示。
2. 改 WM_ERASEBKGND 必须同时检查首帧（创建→显示）与稳态（拖动 / 失效重绘）两个场景，缺一即回归；本类回归验证必须真机开窗看首帧，禁止只看编译通过。
3. 主窗口与取色窗同根因（各有 InvalidateRect(TRUE) 快速重绘路径：对齐虚线框拖动 / SV 色盘拖动），两处改动必须同步评估，不能只改一处。
---

## 2026-10-27 — NSIS 安装段漏杀 BeautypeDeployer.exe → 升级后 deployer 仍是旧版（色块修复"推了但没装上"）

### 现象
v1.1.2 推送了首帧黑白色块修复（settings_window.cpp UpdateWindow），用户在本机覆盖安装 v1.1.2 后，打开设置窗**仍然有色块**。排查发现：安装目录 C:\Program Files (x86)\Beautype\BeautypeDeployer.exe 时间戳仍是 01:02（v1.1.1 旧版），未被 NSIS 覆盖。BeautypeServer.exe / beautype_tsf.dll 都是新的（05:43/05:51），唯独 deployer 没更新。

### 根因
install.nsi 的升级段（.onInit）、主安装段（Section）、卸载段（Uninstall）三处 taskkill 列表里有：BeautypeServer.exe、rime_server.exe、pet_preview.exe、register_tip.exe /u——**唯独没有 BeautypeDeployer.exe**。用户开着设置窗时，BeautypeDeployer.exe 常驻并锁定自身文件，NSIS File 用 SetOverwrite try（try = 失败不报错、静默跳过），新 deployer 写不进去就留旧版。设置窗是 deployer 拉起的，用户看到的一直是旧二进制，源码修复等于白推。

### 防再犯规则
1. **所有会常驻的 exe（server / deployer / pet_preview / register_tip / 任何独立进程）在 NSIS 安装段写文件前都必须 taskkill /f**，缺一不可。新增可执行组件时必须同步加到三处 taskkill 列表。
2. NSIS SetOverwrite try 的含义是"写失败静默跳过"——升级后必须核对安装目录关键 exe 的时间戳/版本号，不能只看安装包构建成功。
3. "源码已修复 + 用户仍复现"时，先查安装目录实际文件版本，再怀疑代码——推了≠装上了。

## 2026-10-27 v1.1.3：开机桌宠角色图消失（preset 覆盖 theme.yaml 时图片路径解析错误）

**现象**：每次开机后右侧桌宠角色图不显示，必须手动打开设置面板点一次"保存"才恢复。日志显示开机时 `[role] FAIL bmp load`，wpath 为 `config\presets\skins\role_xxx.png`（多了 presets\）。

**根因**：server_main.cpp 启动流程先正确加载 `config/theme.yaml`（图片路径解析到 `config/skins/`），但因 `user_theme.saved` 存在，又进入 preset 覆盖逻辑，从 `presets/` 目录加载最新的 `bt兔.yaml`。该文件也写了 `role_image: "skins\role_xxx.png"`，但 ResolveImagePath 以 preset 文件所在目录（presets/）为基准，解析成 `presets/skins/role_xxx.png`——图片实际在 `config/skins/`，不存在。LoadThemeFromYaml 成功返回后 `theme = saved` 覆盖了正确的 theme。手动保存时文件监听直接读 `config/theme.yaml`，路径正确，所以保存后恢复。

**防再犯规则**：
1. 从 presets/ 目录加载 preset 做颜色覆盖时，必须保留从 config/theme.yaml 正确解析的图片路径（background_image / role_image / frame_image），preset 只覆盖颜色/字体/布局/模式等非路径字段。
2. 凡是"从 A 文件加载但相对路径应基于 B 目录解析"的场景，不能依赖 ResolveImagePath 自动推断，必须显式修正。
3. 开机启动路径与运行时文件监听路径是两条独立加载链，改任一条后必须验证另一条不被覆盖逻辑破坏。
