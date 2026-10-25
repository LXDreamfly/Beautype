// Beautype PetHost 最小版（M3-1 预览 v2）：透明置顶分层窗口 + D3D11 渲染 rive 桌宠
// 用法: pet_preview.exe <riv 路径>
// 功能: 右下角显示桌宠(30fps) | 滚轮缩放 200-800px | 左键拖动 | 鼠标悬停可交互/离开穿透 | 右键菜单退出
// 后续 M3-1 完整版在此基础上加: 静音/切换场景/上传/输入法事件链路
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commdlg.h> // GetOpenFileNameW（上传本地 riv）
#include <shlobj.h>   // §52 SHGetKnownFolderPath（Known Folder API，替代 LOCALAPPDATA 环境变量）
#include <d3d11.h>
#include <mmdeviceapi.h>   // WASAPI：IMMDeviceEnumerator/IMMDevice（musicon 检测）
#include <audioclient.h>   // WASAPI：AudioSessionState / IAudioClient
#include <audiopolicy.h>   // WASAPI：IAudioSessionManager2/Enumerator/Control2
#include <endpointvolume.h> // WASAPI：IAudioMeterInformation（会话级峰值）
#include <gdiplus.h>
#include <cstdio>
#include <cstdint>
#include <cctype>   // §57 std::tolower（语言文件小写化判定）
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <algorithm>
#include <map>
#include <set>

#include "rive/file.hpp"
#include "rive/artboard.hpp"
#include "rive/animation/state_machine.hpp"
#include "rive/animation/state_machine_input.hpp"
#include "rive/animation/state_machine_bool.hpp"
#include "rive/animation/state_machine_number.hpp"
#include "rive/animation/state_machine_trigger.hpp"
#include "rive/animation/state_machine_listener.hpp"
#include "rive/animation/listener_input_change.hpp"
#include "rive/animation/listener_bool_change.hpp"
#include "rive/animation/listener_number_change.hpp"
#include "rive/animation/listener_trigger_change.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/audio/audio_engine.hpp"
#include "rive/audio/audio_source.hpp"
#include "rive/audio/audio_sound.hpp"
#include "miniaudio.h" // ma_engine_set_volume（静音）
// Rive 头文件自包含性缺陷: 非 RIVE_CANVAS 构建需显式 include texture.hpp
#include "rive/renderer/texture.hpp"
#include "rive/renderer/rive_renderer.hpp"
#include "rive/renderer/d3d11/render_context_d3d_impl.hpp"

#define PET_WIN_CLASS L"BeautypePetPreview"
#define PET_MARGIN 20
#define PET_FPS_MS 33
#define PET_MIN_SIZE 200
#define PET_MAX_SIZE 800
#define PET_SIZE_STEP 20

// 出厂默认桌宠皮肤文件名（统一约定，勿散落硬编码）。
// 以后想换默认动画：把新做好的 .riv 命名为 beautype_pet.riv 替换即可，无需改代码。
// 改名时全局搜索 kDefaultRivName 同步（server/text_service/pet_preview/install.nsi）。
static const wchar_t kDefaultRivName[] = L"beautype_pet.riv";

static HWND g_hwnd = nullptr;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static rive::gpu::RenderContext* g_rc = nullptr;
static rive::gpu::RenderTargetD3D* g_rt = nullptr;
static ID3D11Texture2D* g_tex = nullptr;
static ID3D11Texture2D* g_staging = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HBITMAP g_dib = nullptr;
static HDC g_dib_dc = nullptr;
static BYTE* g_dib_bits = nullptr;
static rive::ArtboardInstance* g_ab = nullptr;
static rive::File* g_file = nullptr;   // 持有 File（切换场景时重建 ArtboardInstance 用）
static std::unique_ptr<rive::StateMachineInstance> g_smi;
static rive::rcp<rive::AudioEngine> g_engine;
static bool g_exit = false;
static bool g_dragging = false;
static bool g_hover = false;   // 鼠标悬停窗口上（可交互）
// §52 当前打开的右键菜单（根=主菜单）：前向声明提前到全局区（renderFrame 的
// 帧耗时诊断日志需要引用；原声明在文件后部 PetMenuPopup 类前）
class PetMenuPopup;
static PetMenuPopup* g_openMenu = nullptr;
static POINT g_dragOff{};
static POINT g_downPos{};     // 按下位置（区分点击/拖动）
static UINT g_winSize = 512;   // 当前窗口边长（§51 DPI 感知后为物理像素）
static UINT g_dpi = 96;        // 进程 DPI（§51：PER_MONITOR_V2 下主屏 DPI）
static double g_dpiScale = 1.0; // DPI/96（§51：窗口/菜单尺寸与旧配置坐标迁移换算）
// 【闪烁 16.24】滚轮缩放合并：快速滚动只记录目标，停顿后由 WM_TIMER 一次重建，
// 避免每格立即重建（连续重建 → 连续闪）
static bool g_resizePending = false;
static UINT g_resizeTarget = 0;
static DWORD g_lastResizeTick = 0;
// 【闪烁 16.29 同机制】缩放两拍冻结：第一拍只换资源+移位置（不渲染、内容冻结），
// 第二拍（下一拍 WM_TIMER）才渲染——SetWindowPos 移动与 ULW 贴位图严格不同帧，
// 彻底避开拖拽修复断言过的同帧竞争（拖拽=移动中冻结、松手下一拍渲染）。
static bool g_resizeRenderPending = false; // 第一拍已完成、第二拍待渲染
static UINT g_resizeAppliedTarget = 0;     // 第一拍已应用的尺寸（滚动中目标再变→重做第一拍）
static int g_restoreX = -1, g_restoreY = -1;  // 热启动窗口位置（-1=未配置→默认右下角）
static int g_petSlot = 0;   // 多开：本实例槽位（配置/锁文件按槽独立）

// 契约输入句柄（场景切换后需重建）——名字/类型精确匹配契约集合才绑定（bindInputs 枚举）：
// {istyping:bool, click:bool, mouse_x:number, mouse_y:number, musicon:bool}
// 契约集合外输入（drag/hover/生气/悬停/cf 及以后任意名字）一律不绑定、不写值——
// 由皮肤内 Hit Area + Listener 自行驱动（皮肤预览即生效，runtime 只枚举看到它们）。
static rive::SMIBool* g_istypingInput = nullptr;   // 打字动画（800ms 消抖）
static rive::SMIBool* g_clickInput = nullptr;      // 点击动画（800ms 保持）
static rive::SMINumber* g_mouseXInput = nullptr;   // 眼睛跟随（-1..1 相对窗口中心）
static rive::SMINumber* g_mouseYInput = nullptr;
static rive::SMIBool* g_musiconInput = nullptr;    // "正在播放音乐"（WASAPI 会话级）
static rive::SMIBool* g_dragInput = nullptr;       // 按住拖拽中（按住身体=true，松开=false）

// ---- musicon 检测（WASAPI 会话级，后台线程）----
// 语义：系统有非通知类音频会话正在出声 → true。排除：系统通知会话
// （IsSystemSoundsSession）、本进程自身会话（桌宠音效不算外部播放）。
// 500ms 去抖：每 250ms 采样一次，连续 2 次一致才翻转（kMusiconDebounceSamples）。
static volatile LONG g_musiconState = 0;   // 去抖后状态（线程写 / 主循环读）
static HANDLE g_musiconThread = nullptr;
static constexpr float kMusiconPeakEpsilon = 0.0005f; // 能量存在下限（≈-66dBFS）；
                                                     // 信号能量≠音量门槛（门槛方案已证伪：
                                                     // App/系统音量与播放状态独立，代表不了"正在播放"）
static constexpr DWORD kMusiconPollMs = 250;
static constexpr int kMusiconDebounceSamples = 2;
static constexpr DWORD kMusiconHoldMs = 6000; // 出声保持窗口（VAD hangover）：一旦
                                             // 出声记时刻，窗口内 peak 跌破不翻转
                                             // （音乐间奏不打断；用户实测抖动根因）
static bool g_muted = false;      // 静音开关
static UINT g_curSmIndex = 0;     // 当前状态机索引
static UINT g_diagFrames = 0;     // 渲染诊断计数
static bool g_ulwWarned = false;  // ULW 失败已警告
static bool g_diagEdge = false;   // 白边诊断模式（--diag-edge：隐藏窗口跑 N 帧后退出）
static UINT g_diagEdgeFrames = 0; // 诊断模式已渲染帧数
static bool g_diagShotRaw = false;     // 已保存阴影前 PNG
static bool g_diagShotShadow = false;  // 已保存阴影后 PNG

// ---- 打字监听（istyping）消抖状态机 ----
// 节奏：任意按键按下 → istyping=true + 空闲计时器重置 800ms；连续打字持续重置、
// 动画连贯；停手 0.8s 自动回待机。严禁"按下=真/松开=假"直连（每键闪一下会抖）。
static bool g_isTyping = false;   // 正在打字（消抖后）
static int g_idleMs = 0;          // 空闲计时（毫秒），每帧递减，<=0 复位
static volatile LONG g_typingPulse = 0; // TCP 线程置 1 通知主循环有按键到来
static const int kTypingIdleMs = 800;   // 停手 0.8s 回待机（用户指定 600-800ms）

// ---- 点击监听（click）消抖状态机 ----
// 节奏：任意左键按下（全局 WH_MOUSE_LL 钩子事件）→ click=true + 空闲计时
// kClickHoldMs → 回 false。与 istyping 同款"脉冲+短保持"，严禁按下=真/松开=假
// 直连（直连会抖；30fps 轮询 GetAsyncKeyState 还会漏掉 <33ms 的快速点击）。
static bool g_clickActive = false;     // 点击脉冲消抖后状态
static int g_clickIdleMs = 0;          // 空闲计时（毫秒），每帧递减，<=0 复位
static volatile LONG g_clickPulse = 0; // 鼠标钩子线程置 1 通知主循环有左键按下
static const int kClickHoldMs = 800;   // 点击动画保持时长（可调 400-800ms）

// ---- M3-3 按键显示 HUD 卡片（复刻 Electron index.html #keyboard-hud） ----
// keydown → 卡片显示（按住保持）；keyup → 延迟隐藏（防快速打字闪烁）。
// 防闪烁设计（对齐成熟按键显示工具 input-overlay/KeyCastr 的 release timeout +
// fade 语义）：keyup 不立即消失，卡片保持 kHudHoldMs=350ms 渐变淡出；同键
// 快速连击时 held 状态持续覆盖，视觉不闪；不同键快速连打则平滑过渡。
// 绘制: GDI+ 直接画到 ULW 位图 DC（见 §16.15：绝不包 DIB 内存改 alpha）。
static ULONG_PTR g_gdipToken = 0;
static CRITICAL_SECTION g_hudLock;                    // IPC 线程写 / 主线程画
static const uint64_t kHudHoldMs = 350;               // keyup 后保持/淡出时长
// 按住超时保护：TSF 只在输入法激活期间转发按键，焦点/输入法切走后 keyup 丢失 →
// 卡片 held=true 永久常驻（对方机器 Ctrl/F/H/Key91 常驻根因，m1 §29）。超此时长
// 仍无 keyup 视为漏发，直接过期擦除；焦点仍在时键重复事件会刷新 down_ms，不受影响。
static const uint64_t kHudMaxHoldMs = 3000;
// 物理真值兜底（2026-10-20 §49）：LL 钩子 keyup 偶发丢失（系统超时静默卸载钩子/
// 失焦/UAC 安全桌面）时，旧方案只靠 kHudMaxHoldMs=3000 超时擦除，且依赖绘制循环
// 持续运行；真长按（物理仍按下）超过 3 秒还会被误擦除。修复：drawKeyHud 每帧对
// held 键查 GetAsyncKeyState 物理真值——物理已松开且 down 停止 ≥ 该时长即判定
// keyup 丢失，转淡出（不依赖 3 秒超时）；物理仍按下则一直显示，不受超时限制。
static const uint64_t kHudKeyupLooseMs = 150;   // keyup 丢失判定：物理松开且 down 停止此时长
// 钩子自愈：HUD 卡住清除（keyup 丢失兜底）累计达到该次数 → 重装 LL 键盘钩子
// （LL 钩子被系统超时静默卸载后不会自动恢复，重装恢复 up 链路，防复发）
static const int kHudKeyupLossRebuildAt = 3;
static int g_hudKeyupLossCount = 0;             // keyup 丢失兜底累计次数（跨键统计）
struct HudKeyState {
    bool held = false;      // 当前按住中
    uint64_t down_ms = 0;   // 最近按下时刻
    uint64_t up_ms = 0;     // 最近抬起时刻（0 = 从未抬起）
    int vk = 0;             // 原始虚拟键码（GetAsyncKeyState 物理真值查询用，§49）
};
static std::map<std::string, HudKeyState> g_hudKeys;  // label -> 状态
static void rebuildKbdHook();   // 前置声明：LL 钩子自愈重装（定义见 M4-1 段，§49）
static ULONG g_gdipStarted = 0;
static bool g_hudEnabled = true;                      // 右键菜单「按键显示」开关（默认开）

static bool g_hasRendered = false;                    // 首帧渲染完成（启动切换不淡化）
static bool g_hasShown = false;                       // 首次"出画"才显示（跳过 SM 开场黑场）

// ---- 场景切换"自然淡出过渡"（用户指定：旧画面自然慢慢消散 → 新场景渐渐出现） ----
// 依据通用动效最佳实践（Material Design fade-through / Android 叠化 / 动效时长
// 100-500ms 区间）：退场 ease-out 缓动（开始快、结尾慢慢拖，消散感）、
// 入场 ease-out（渐渐浮现）。时长可调：kFadeInMs=交叉淡化时长。
// 【黑屏根因重设计】旧方案 FadeOut 把旧画面淡出到全透明 → 窗口透明 → 桌面透出
// （深色桌面=黑屏闪烁 <1s）。改为交叉淡化：新皮出画前保持旧快照全 alpha（窗口不消失），
// 出画后旧快照→新帧直接渐变，全程无透明间隙。
enum class PetFadePhase { None, Waiting, Crossfade };
static PetFadePhase g_fadePhase = PetFadePhase::None;
static DWORD g_fadePhaseStart = 0;
static std::vector<uint32_t> g_fadeShot;              // 切换瞬间旧画面快照（BGRA/DIB 序）
// 【过场裁剪·出画判定 2026-10-19 用户定稿】与启动裁剪 g_hasShown 同款：新皮 readback
// 的 RGB 内容像素占比 > 总像素 2% 即"已出画"——纯内容占比判定，与 riv 过场时长无关
// （任意 riv 自适应，不写死时间/帧数/稳定帧）。等待期显示过渡加载动画（DotsJump），
// 绝不显示 rive 开场黑场/黑屏；未出画就持续等待，无超时强制（8s 强制会把黑帧放出）。
static constexpr DWORD kFadeInMs = 400;               // 旧→新交叉淡化时长（用户可调）

// ---- 过渡期加载动画（定稿唯一方案：DotsJump 圆点跳动，2026-10-15 用户指定替换） ----
// DotsJump = 圆点跳动：四瓣白实心圆错峰上下弹跳（左上0/右上150/左下300/右下450ms，1.2s 一轮）
// 每瓣同时明暗呼吸（灰度 176..244）；仅在 FadeIn 等待期（新场景未出画）绘制；出画帧起停止，衔接 rive 淡入。
// 早期对比方案 FlapBreath（扇动呼吸）已按用户要求替换移除。
static bool g_loadingDraw = false;
static DWORD g_loadingStart = 0;
static UINT g_ldDiagFrames = 0;   // 加载动画绘制诊断

// ---- 启动加载过场（复用 DotsJump）：riv 解析在后台线程，主线程持续画加载动画 ----
// 加载链（import/artboard/状态机/音频）是纯 CPU 操作，可安全放后台线程：
// File::import 只解析字节（Factory 仅存储、绘制时才调用，见 rive file.cpp:409）；
// ArtboardInstance 构造为空（artboard.cpp:4810）、StateMachineInstance 构造是纯逻辑。
static bool g_loadOk = false;      // 加载线程成功完成（riv/artboard/状态机/音频就绪）
static DWORD g_loadDoneTick = 0;   // 加载完成时刻（首显超时兜底从此刻起算，加载慢不误超时）

// 调试: 自动切换状态机（复现"切换后渲染全空"）argv[3]=秒数 argv[4]=目标SM
// argv[5]=第二次切换秒数 argv[6]=目标SM（判别"artboard 被破坏" vs "SM1 特定"）
static UINT g_autoSwitchSec = 0;
static UINT g_autoSwitchTarget = 0;
static UINT g_autoSwitchSec2 = 0;
static UINT g_autoSwitchTarget2 = 0;
static DWORD g_autoSwitchStart = 0;
static bool g_autoSwitchDone = false;
static UINT g_frameNumber = 0;   // PLS currentFrameNumber 递增（官方惯例）

// ---- 多皮肤库：%LOCALAPPDATA%\rime_beautify\pet\skins\ ----
// 上传的每个 riv 以原文件名入库存档（重名自动加序号，不覆盖）；config 记当前
// 生效皮肤名（""=出厂默认）。恢复默认设置=皮肤/静音/按键显示/场景全部回出厂，
// 皮肤文件一律保留可随时切回（用户定稿，2026-10-15）。
static std::string g_skinName;        // 当前生效皮肤文件名（""=出厂默认）
static std::wstring g_defaultSkinPath; // 出厂默认皮肤绝对路径（CLI 参数 > exe 同目录默认 riv）

static FILE* g_logFile = nullptr;   // 产品化：日志落文件（%LOCALAPPDATA%\rime_beautify\logs\pet_preview.log）
// 后台加载线程与主线程都会写日志 → FILE* 并发写会交错/撕裂，加临界区串行化。
static CRITICAL_SECTION g_logLock;
static bool g_logLockReady = false; // main 最早初始化；就绪前只写 stderr 不需要锁

// ---- 日志分级（业界对齐 glog：DEBUG < INFO < WARN < ERROR） ----
// 级别过滤：行级别 < g_logLevel 的行不落盘。
// 默认级别随构建走：测试构建（未定义 BEAUTYPE_RELEASE）全量 0；
// 发布构建（/DBEAUTYPE_RELEASE）INFO 起 1。运行时被 pet_config.<n>.json
// 的 log_level 字段覆盖（0=全量 1=INFO 2=WARN 3=仅 ERROR），排障开 0 复现。
enum { kLogDebug = 0, kLogInfo = 1, kLogWarn = 2, kLogError = 3 };
#ifdef BEAUTYPE_RELEASE
static const int kDefaultLogLevel = kLogWarn;   // 发布：只打关键位置报错（WARN/ERROR）
#else
static const int kDefaultLogLevel = kLogDebug;  // 测试：全量
#endif
static int g_logLevel = kDefaultLogLevel;

// 行前缀白名单分级（覆盖本文件全部 83 处 log() 调用，逐行核对过）：
// [FAIL]/上传复制失败/删除失败 → ERROR；重载失败/配置回退/钩子安装失败/
// ULW 合成失败/IPC 断线重连/上传校验未通过 → WARN；每键脉冲/逐步探针/
// 渲染诊断/自动切换/加载过场 → DEBUG；其余（OK/配置/交互/皮肤生效/菜单/
// 上传入库/删除成功/IPC 连接/启动链）→ INFO。
// 新增日志请沿用此前缀约定，级别自动归类。
static int logLevelFromPrefix(const char* fmt)
{
    // ERROR（功能失败）
    if (strncmp(fmt, "[FAIL]", 6) == 0) return kLogError;
    if (strncmp(fmt, "[上传] 复制失败", strlen("[上传] 复制失败")) == 0) return kLogError;
    if (strncmp(fmt, "[删除] 删除失败", strlen("[删除] 删除失败")) == 0) return kLogError;
    // WARN（意外但已处理 / 回退默认）
    if (strncmp(fmt, "[皮肤] 重载失败", strlen("[皮肤] 重载失败")) == 0) return kLogWarn;
    if (strncmp(fmt, "[皮肤] 配置皮肤", strlen("[皮肤] 配置皮肤")) == 0) return kLogWarn;
    if (strncmp(fmt, "[输入] 键盘钩子安装失败", strlen("[输入] 键盘钩子安装失败")) == 0) return kLogWarn;
    if (strncmp(fmt, "[输入] 鼠标钩子安装失败", strlen("[输入] 鼠标钩子安装失败")) == 0) return kLogWarn;
    if (strncmp(fmt, "[输入] musicon 检测失败", strlen("[输入] musicon 检测失败")) == 0) return kLogWarn;
    if (strncmp(fmt, "[加载] 契约输入", strlen("[加载] 契约输入")) == 0) return kLogWarn;
    if (strncmp(fmt, "[加载] 输入未驱动", strlen("[加载] 输入未驱动")) == 0) return kLogWarn;
    if (strncmp(fmt, "[渲染] ULW 合成失败", strlen("[渲染] ULW 合成失败")) == 0) return kLogWarn;
    if (strncmp(fmt, "[IPC] 连接断开", strlen("[IPC] 连接断开")) == 0) return kLogWarn;
    if (strncmp(fmt, "[上传] 文件校验未通过", strlen("[上传] 文件校验未通过")) == 0) return kLogWarn;
    // DEBUG（每键/每帧/逐步探针/诊断，测试全量可见、发布默认关）
    if (strncmp(fmt, "[输入] 打字", strlen("[输入] 打字")) == 0) return kLogDebug;
    if (strncmp(fmt, "[输入] 停手", strlen("[输入] 停手")) == 0) return kLogDebug;
    if (strncmp(fmt, "[输入] 点击", strlen("[输入] 点击")) == 0) return kLogDebug;
    if (strncmp(fmt, "[步进]", 5) == 0) return kLogDebug;
    if (strncmp(fmt, "[诊断]", 5) == 0) return kLogDebug;
    if (strncmp(fmt, "[调试]", 5) == 0) return kLogDebug;
    if (strncmp(fmt, "[加载]", 5) == 0) return kLogDebug;
    if (strncmp(fmt, "[渲染] 非空", strlen("[渲染] 非空")) == 0) return kLogDebug;
    return kLogInfo;
}

static void log(const char* fmt, ...)
{
    const int level = logLevelFromPrefix(fmt);
    if (level < g_logLevel) return;  // 分级过滤：发布默认 1 起，DEBUG 行不落盘
    va_list ap;
    const bool locked = g_logLockReady;
    if (locked) EnterCriticalSection(&g_logLock);
    if (g_logFile) {
        // 行首时间戳 [HH:MM:SS.mmm]（对齐 IME LogF：跨日志对齐时序，
        // 桌宠/输入法服务/TSF 三份日志能按同一时钟串起事件）。
        // 注意：本函数每次调用恰好写一行（fmt 无内嵌换行，83 处核对过），
        // 因此无条件写时间戳；勿在 fmt 内嵌 '\n' 续行（会插到行中间）。
        // 不用 fseek/fgetc 回读行首判断——实测 "ab" 追加模式下仅首行生效。
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char ts[32] = {};
        snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03d] ",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        fputs(ts, g_logFile);
        va_start(ap, fmt);
        vfprintf(g_logFile, fmt, ap);
        va_end(ap);
        fputc('\n', g_logFile);
        fflush(g_logFile);
    }
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    if (locked) LeaveCriticalSection(&g_logLock);
}

// ---- 契约输入集合（2026-10-19 重构：取代逐个 getXxx 硬编码）----
// 契约 = runtime 必须写值、名字完全对齐、精确匹配：{istyping:bool, click:bool,
// mouse_x:number, mouse_y:number, musicon:bool}。集合外输入（drag/hover/生气/
// 悬停/cf 及以后任意名字）一个不碰——皮肤内 Hit Area + Listener 自行驱动
// （皮肤预览即生效，runtime 只需枚举看到它们）。
enum class ContractType { Bool, Number, Trigger };
struct ContractInput {
    const char* name;
    ContractType type;
    rive::SMIBool** boolSlot = nullptr;      // 绑定目标（按类型选一）
    rive::SMINumber** numberSlot = nullptr;
};
static ContractInput kContractInputs[] = {
    { "istyping", ContractType::Bool,   &g_istypingInput, nullptr },
    { "click",    ContractType::Bool,   &g_clickInput,    nullptr },
    { "mouse_x",  ContractType::Number, nullptr,          &g_mouseXInput },
    { "mouse_y",  ContractType::Number, nullptr,          &g_mouseYInput },
    { "musicon",  ContractType::Bool,   &g_musiconInput,  nullptr },
    { "drag",     ContractType::Bool,   &g_dragInput,     nullptr },
};

static const char* smiTypeName(uint16_t coreType)
{
    if (coreType == rive::StateMachineBoolBase::typeKey) return "bool";
    if (coreType == rive::StateMachineNumberBase::typeKey) return "number";
    if (coreType == rive::StateMachineTriggerBase::typeKey) return "trigger";
    return "unknown";
}
static ContractInput* findContract(const std::string& name)
{
    for (auto& c : kContractInputs) if (name == c.name) return &c;
    return nullptr;
}
// 契约期望类型是否与皮肤实际 coreType 一致
static bool contractTypeMatches(const ContractInput& c, uint16_t coreType)
{
    switch (c.type)
    {
        case ContractType::Bool:   return coreType == rive::StateMachineBoolBase::typeKey;
        case ContractType::Number: return coreType == rive::StateMachineNumberBase::typeKey;
        case ContractType::Trigger:return coreType == rive::StateMachineTriggerBase::typeKey;
    }
    return false;
}

// 绑定状态机输入：枚举 + 契约集合精确绑定（取代逐个 getXxx 硬编码）。
// 遍历全部输入（名字 + 类型），仅对精确匹配契约集合的名字+类型做绑定；其余只
// 记录不绑定。加载日志每行一个输入（名字 + 类型 + 接管状态：契约接管 /
// riv 内部监听器 / 未驱动告警）——皮肤作者一眼看出哪个输入没有数据源。
static void bindInputs()
{
    g_istypingInput = g_clickInput = g_musiconInput = g_dragInput = nullptr;
    g_mouseXInput = g_mouseYInput = nullptr;
    if (!g_smi) return;
    const rive::StateMachine* sm = g_ab ? g_ab->stateMachine(g_curSmIndex) : nullptr;

    // 1) 收集"被 riv 内部监听器引用"的输入名（Hit Area + Listener 动作 → 输入变更；
    //    inputId 即 StateMachine::input(index) 索引——官方 listener_*.cpp 同款用法）
    std::set<std::string> listenerDriven;
    if (sm)
    {
        for (size_t l = 0; l < sm->listenerCount(); l++)
        {
            const rive::StateMachineListener* lst = sm->listener(l);
            if (!lst) continue;
            for (size_t a = 0; a < lst->actionCount(); a++)
            {
                const rive::ListenerAction* act = lst->action(a);
                if (!act) continue;
                // coreType 分发（不用 dynamic_cast：rive.lib /GR- 编译，RTTI 越界崩溃）
                uint16_t ct = act->coreType();
                const rive::ListenerInputChange* lic = nullptr;
                if (ct == rive::ListenerBoolChangeBase::typeKey)
                    lic = static_cast<const rive::ListenerBoolChange*>(act);
                else if (ct == rive::ListenerNumberChangeBase::typeKey)
                    lic = static_cast<const rive::ListenerNumberChange*>(act);
                else if (ct == rive::ListenerTriggerChangeBase::typeKey)
                    lic = static_cast<const rive::ListenerTriggerChange*>(act);
                if (!lic) continue;
                const rive::StateMachineInput* ti = sm->input((size_t)lic->inputId());
                if (ti) listenerDriven.insert(ti->name());
            }
        }
    }

    // 2) 枚举实例侧全部输入：名字 + 类型 + 接管状态
    const size_t n = g_smi->inputCount();
    int contractOk = 0, contractWarn = 0;
    std::set<std::string> seenContract;
    for (size_t i = 0; i < n; i++)
    {
        const rive::SMIInput* inp = g_smi->input(i);
        if (!inp) continue;
        const std::string name = inp->name();
        const uint16_t ct = inp->inputCoreType();
        const char* status = "riv 内部监听器";
        ContractInput* c = findContract(name);
        if (c)
        {
            seenContract.insert(name);
            if (contractTypeMatches(*c, ct))
            {
                // input(i) 返回 const 指针防改输入列表；输入对象本身可变
                // （官方 getBool/getNumber 内部同样 const_cast 后返回非 const 句柄）
                if (c->boolSlot) *c->boolSlot = static_cast<rive::SMIBool*>(const_cast<rive::SMIInput*>(inp));
                else if (c->numberSlot) *c->numberSlot = static_cast<rive::SMINumber*>(const_cast<rive::SMIInput*>(inp));
                status = "契约接管";
                contractOk++;
            }
            else
            {
                status = "契约类型不符(未接管)";
                contractWarn++;
                log("[加载] 契约输入 \"%s\" 类型不符：期望 %s，皮肤实为 %s → 该输入无数据源 / 动画不生效",
                    name.c_str(), c->type == ContractType::Bool ? "bool" :
                    c->type == ContractType::Number ? "number" : "trigger", smiTypeName(ct));
            }
        }
        else
        {
            if (!listenerDriven.count(name))
            {
                status = "未驱动告警";
                log("[加载] 输入未驱动：\"%s\"(%s) 非契约且无监听器引用 → 该输入无数据源 / 动画不生效",
                    name.c_str(), smiTypeName(ct));
            }
        }
        log("[加载] 输入[%zu] name=\"%s\" type=%s 接管=%s", i, name.c_str(), smiTypeName(ct), status);
    }
    // 3) 契约输入在皮肤中缺位 → 明确告警（禁止静默）
    for (auto& c : kContractInputs)
    {
        if (!seenContract.count(c.name))
        {
            contractWarn++;
            log("[加载] 契约输入缺失：\"%s\" 皮肤未导出 → 该输入无数据源 / 动画不生效",
                c.name);
        }
    }
    log("[OK] 输入枚举: 共 %zu 个，契约接管 %d，告警 %d（非契约输入由皮肤 Hit Area+Listener 内部驱动）",
        n, contractOk, contractWarn);
}

// ---- 桌宠状态持久化（静音/按键显示/当前场景 → pet_config.json，热启动恢复） ----
// 极简 JSON 取值（固定字段，不引入第三方 JSON；IPC 客户端也复用）：
// "key":value 或 "key":"value"
static long long jsonNum(const std::string& s, const char* key)
{
    std::string k = std::string("\"") + key + "\":";
    size_t p = s.find(k);
    if (p == std::string::npos) return -1;
    p += k.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    long long v = 0; bool neg = false;
    if (p < s.size() && s[p] == '-') { neg = true; p++; }
    while (p < s.size() && isdigit((unsigned char)s[p])) { v = v * 10 + (s[p] - '0'); p++; }
    return neg ? -v : v;
}
static std::string jsonStr(const std::string& s, const char* key)
{
    std::string k = std::string("\"") + key + "\":\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return {};
    p += k.size();
    size_t e = s.find('"', p);
    if (e == std::string::npos) return {};
    return s.substr(p, e - p);
}

// 【§57 英文菜单】读桌宠界面语言：复用输入法同一份 %APPDATA%\Beautype\config\language
// （Roaming，zh/en，缺省 zh）。注意：桌宠现有路径全在 LocalAppData
// （%LOCALAPPDATA%\rime_beautify），而语言文件在 Roaming（FOLDERID_RoamingAppData），
// 必须走 Known Folder Roaming API 取 Roaming 目录，不能复用下面的 knownLocalAppData。
// 判定语义与 IME strings.cpp LoadLanguage 对齐：读文件 → 小写化 → 含 "en" 即英文，否则中文。
static bool petIsEnglish()
{
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)))
        return false;
    std::wstring roaming = p ? p : L"";
    CoTaskMemFree(p);
    if (roaming.empty()) return false;
    std::wstring path = roaming + L"\\Beautype\\config\\language";
    std::ifstream f(path.c_str());
    if (!f) return false; // 缺省中文
    std::string body;
    std::getline(f, body);
    std::transform(body.begin(), body.end(), body.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return body.find("en") != std::string::npos;
}

// ---- 多皮肤库路径与枚举（皮肤名在 config 里存 UTF-8，落盘用 UTF-16） ----
static std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 1 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
static std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 1 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}
// 【§52】取用户本地数据目录：SHGetKnownFolderPath(FOLDERID_LocalAppData) 替代
// GetEnvironmentVariableW(L"LOCALAPPDATA")。根因：环境变量可被清空/异常（对方电脑
// 用户名含 & 时 cmd 显示截断，且环境变量本身可能缺失），Known Folder API 是微软
// 权威取路径方式（读系统已知文件夹表，不经过环境变量），Electron/VPet 底层均走此
// API。失败返回空串（与旧行为一致）；返回的 PWSTR 必须 CoTaskMemFree 释放。
static std::wstring knownLocalAppData()
{
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
        return L"";
    std::wstring s = p ? p : L"";
    CoTaskMemFree(p);
    return s;
}
static std::wstring petSkinsDir()
{
    std::wstring base = knownLocalAppData();
    if (base.empty()) return L"";
    return base + L"\\rime_beautify\\pet\\skins\\";
}
static std::wstring petSkinPath(const std::string& name)
{
    return petSkinsDir() + utf8ToWide(name);
}
static std::vector<std::string> listSkins()
{
    std::vector<std::string> out;
    const std::wstring dir = petSkinsDir();
    if (dir.empty()) return out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.riv").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            out.push_back(wideToUtf8(std::wstring(fd.cFileName)));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

// ===== 多开：槽位锁（runtime\pet.lock.<n> 记录 PID；活槽=PID 进程存在） =====
static std::wstring petRuntimeDir()
{
    std::wstring base = knownLocalAppData();
    if (base.empty()) return L"";
    std::wstring d = base + L"\\rime_beautify\\runtime";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}
static std::wstring petLockPath(int slot)
{
    return petRuntimeDir() + L"\\pet.lock." + std::to_wstring(slot);
}
static bool pidAlive(DWORD pid)
{
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    CloseHandle(h);
    return true;
}
static int acquirePetSlot()
{
    for (int n = 0; n < 100; ++n)
    {
        const std::wstring lp = petLockPath(n);
        if (GetFileAttributesW(lp.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            // 锁存在：读 PID，活着=占用中；死了=复用
            std::ifstream f(lp, std::ios::binary);
            DWORD pid = 0;
            if (f >> pid && pidAlive(pid)) continue;
        }
        std::ofstream(lp, std::ios::binary | std::ios::trunc) << GetCurrentProcessId();
        return n;
    }
    return 0;
}
static int countLivePetSlots()
{
    int live = 0;
    const std::wstring dir = petRuntimeDir();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\pet.lock.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        DWORD pid = 0;
        std::wstring lp = dir + L"\\" + fd.cFileName;
        std::ifstream f(lp, std::ios::binary);
        if (f >> pid && pidAlive(pid)) ++live;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return live;
}

// 【窗口屏幕外修复 2026-10-19 §48】窗口中心点是否落在任一显示器工作区内（可见性
// 兜底校验）。恢复位置 / 多开错开若把窗口推出屏幕，用户看不到桌宠（"双击没反应"），
// 且退出/拖动/缩放时会把坏位置持久化进 pet_config.<n>.json → 下次热启动恢复坏位置
// 永久屏外。本机实锤：槽 16-20 配置存了 (668,-500)~(348,-820) 负坐标。
static bool windowCenterOnAnyScreen(int x, int y, int w, int h)
{
    const POINT c{ x + w / 2, y + h / 2 };
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi))
    {
        return c.x >= mi.rcWork.left && c.x < mi.rcWork.right &&
               c.y >= mi.rcWork.top && c.y < mi.rcWork.bottom;
    }
    return false;
}

static std::wstring petConfigWritePath()
{
    std::wstring base = knownLocalAppData();
    if (base.empty()) return L"";
    return base + L"\\rime_beautify\\pet_config." + std::to_wstring(g_petSlot) + L".json";
}
// 读：优先本槽文件；槽0 首次升级前回退旧扁平 pet_config.json（迁移零感知）
static std::wstring petConfigPath()
{
    std::wstring base = knownLocalAppData();
    if (base.empty()) return L"";
    std::wstring slotPath = base + L"\\rime_beautify\\pet_config." + std::to_wstring(g_petSlot) + L".json";
    if (g_petSlot == 0 && GetFileAttributesW(slotPath.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        std::wstring legacy = base + L"\\rime_beautify\\pet_config.json";
        if (GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES)
            return legacy;
    }
    return slotPath;
}
static void savePetConfig()
{
    const std::wstring path = petConfigWritePath();   // 多开：写本槽独立文件
    if (path.empty()) return;
    const std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
    CreateDirectoryW(dir.c_str(), nullptr);
    std::string sm;
    if (g_ab && g_curSmIndex < g_ab->stateMachineCount())
        sm = g_ab->stateMachine(g_curSmIndex)->name();
    std::string json = "{\"hud_enabled\":" + std::to_string(g_hudEnabled ? 1 : 0);
    json += ",\"muted\":" + std::to_string(g_muted ? 1 : 0);
    json += ",\"state_machine\":\"" + sm + "\"";
    // skin 文件名不含引号/反斜杠（Windows 文件名禁止），无需转义
    json += ",\"skin\":\"" + g_skinName + "\"";
    // 日志级别写回当前值（用户手动设的 log_level 不被本保存冲掉）
    json += ",\"log_level\":" + std::to_string(g_logLevel);
    // 热启动窗口状态：缩放比例(窗口边长) + 桌面位置(左上角)；右键退出/拖动结束/缩放即持久化
    if (g_hwnd)
    {
        RECT wr{};
        GetWindowRect(g_hwnd, &wr);
        // §51：win_* 为物理像素（DPI 感知后 GetWindowRect 物理）；coord_phys=1 标记
        // 供 loadWindowStateFromConfig 区分旧版（DPI-unaware 逻辑像素）一次性迁移，
        // 避免升级后窗口位置偏移/缩小、以及二次放大
        json += ",\"coord_phys\":1";
        json += ",\"win_size\":" + std::to_string(g_winSize);
        json += ",\"win_x\":" + std::to_string(wr.left);
        json += ",\"win_y\":" + std::to_string(wr.top);
    }
    json += "}";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f << json;
}
// 加载桌宠状态：按键显示/静音直接应用；场景按状态机名匹配（文件变了回退 CLI/默认）
static void loadPetConfig(size_t* initialSm, size_t smCount)
{
    const std::wstring path = petConfigPath();
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (s.empty()) return;
    g_hudEnabled = jsonNum(s, "hud_enabled") != 0;
    g_muted = jsonNum(s, "muted") != 0;
    // 日志级别覆盖（0=全量 1=INFO 2=WARN 3=仅ERROR；缺省 -1 保持编译默认：
    // 测试 0 全量 / 发布 1 INFO 起；排障时改 0 重启即全量复现）
    const long long lv = jsonNum(s, "log_level");
    if (lv >= kLogDebug && lv <= kLogError) g_logLevel = (int)lv;
    // 旧配置里的 loading_style 键忽略（过场已定稿唯一方案 DotsJump，键不再写）
    const std::string sm = jsonStr(s, "state_machine");
    if (!sm.empty() && smCount > 0)
    {
        for (size_t i = 0; i < smCount; ++i)
        {
            if (g_ab->stateMachine(i)->name() == sm)
            {
                *initialSm = i;
                break;
            }
        }
    }
    log("[配置] 已恢复: 按键显示=%s 静音=%s 场景=\"%s\"",
        g_hudEnabled ? "开" : "关", g_muted ? "开" : "关", sm.c_str());
}
// 读取热启动皮肤名（config.skin；无键=出厂默认）。加载哪个文件在 wWinMain
// 决定（皮肤决定文件），场景/静音/按键显示在加载线程 loadPetConfig 恢复。
static void loadSkinNameFromConfig()
{
    const std::wstring path = petConfigPath();
    std::ifstream f(path, std::ios::binary);
    if (!f) { g_skinName.clear(); return; }
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    g_skinName = jsonStr(s, "skin"); // 老配置无 skin 键 → 空 = 出厂默认（零迁移）
}
// 【§51 DPI 感知修复】对齐 rime_server server_main.cpp L410 的
// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 做法，全部动态加载：本项目未定义
// _WIN32_WINNT（§41 教训），直接引用 DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 /
// GetDpiForSystem 的编译期常量与导入不可靠，GetProcAddress 规避宏依赖。
// 根因（对方 2560x1440 右键菜单"能显示但交互不了"）：本进程 DPI-unaware 时
// GetWindowRect/GetCursorPos 返回虚拟化逻辑坐标，而 WH_MOUSE_LL 钩子 ms->pt 恒为
// 物理屏幕坐标（LowLevelMouseProc L1950-1951 直接比较两坐标系）→ DPI>100% 时
// 菜单外点击被误判 inside=true → 菜单不响应点击也不关闭。
// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((DPI_AWARENESS_CONTEXT)-4)
static void enableDpiAwareness()
{
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI* SetProcessDpiAwarenessContext_t)(HANDLE);
    typedef BOOL(WINAPI* SetProcessDPIAware_t)(void);
    typedef UINT(WINAPI* GetDpiForSystem_t)(void);
    SetProcessDpiAwarenessContext_t dpiCtx =
        (SetProcessDpiAwarenessContext_t)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    GetDpiForSystem_t dpiForSystem =
        (GetDpiForSystem_t)GetProcAddress(u32, "GetDpiForSystem");
    if (dpiCtx && dpiCtx((HANDLE)-4))          // PER_MONITOR_AWARE_V2（Win10 1607+）
    {
        g_dpi = dpiForSystem ? dpiForSystem() : 96;
    }
    else
    {
        // 回退系统感知（Win8.1+ SetProcessDPIAware）：非 PerMonitor 但同为 DPI-aware，
        // 进程内坐标仍全为物理像素，菜单命中判定同样回到同一坐标系
        SetProcessDPIAware_t dpiAware =
            (SetProcessDPIAware_t)GetProcAddress(u32, "SetProcessDPIAware");
        if (dpiAware) dpiAware();
        HDC dc = GetDC(nullptr);
        if (dc) { g_dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); }
        else g_dpi = 96;
    }
    if (g_dpi == 0) g_dpi = 96;
    g_dpiScale = (double)g_dpi / 96.0;
    log("[DPI] awareness=PER_MONITOR_AWARE_V2 dpi=%u scale=%.2f", g_dpi, g_dpiScale);
}
// 读取热启动窗口状态（config.win_size/win_x/win_y）。窗口创建前调用；
// 无效/缺失回退默认（右下角 + 出厂 512）。键是否存在用子串判断（win_x=0 是合法位置）。
static void loadWindowStateFromConfig()
{
    const std::wstring path = petConfigPath();
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // §51 迁移：新配置（coord_phys 键存在）win_* 为物理像素直接使用；旧版（无键，
    // DPI-unaware 时代写入）为逻辑像素 → 一次性乘 g_dpiScale 迁到物理，防止
    // 升级后窗口位置偏左上/尺寸缩小；迁移后下次 savePetConfig 落物理值+标记，
    // 不会二次放大（loadWindowStateFromConfig 仅启动读一次）。
    // 注意：必须用"键是否存在"判断，不能用 jsonNum!=0——jsonNum 对缺失键返回 -1，
    // -1!=0 恒真会把旧配置误判为物理坐标（实机迁移验证抓到的 bug）
    const bool coordPhys = s.find("\"coord_phys\"") != std::string::npos;
    if (s.find("\"win_size\"") != std::string::npos)
    {
        long long sz = jsonNum(s, "win_size");
        long long px = coordPhys ? sz : (long long)(sz * g_dpiScale);
        if (px >= PET_MIN_SIZE && px <= PET_MAX_SIZE) g_winSize = (UINT)px;
    }
    else if (!coordPhys)
    {
        // 出厂默认：逻辑 512 → 物理（保持"512 逻辑"的视觉大小，超上限截断）
        long long px = (long long)(512 * g_dpiScale);
        g_winSize = (UINT)std::max<long long>(PET_MIN_SIZE,
                                              std::min<long long>(PET_MAX_SIZE, px));
    }
    if (s.find("\"win_x\"") != std::string::npos && s.find("\"win_y\"") != std::string::npos)
    {
        long long wx = jsonNum(s, "win_x"), wy = jsonNum(s, "win_y");
        if (coordPhys)
        {
            if (wx >= 0 && wy >= 0) { g_restoreX = (int)wx; g_restoreY = (int)wy; }
        }
        else
        {
            // 旧配置逻辑坐标 → 物理；屏外由 wWinMain 的 §48 windowCenterOnAnyScreen
            // 兜底回退右下角（坏位置不会二次持久化）
            wx = (long long)(wx * g_dpiScale);
            wy = (long long)(wy * g_dpiScale);
            if (wx >= 0 && wy >= 0) { g_restoreX = (int)wx; g_restoreY = (int)wy; }
        }
    }
    log("[配置] 热启动窗口: %ux%u @ (%d,%d)（coord_phys=%s%s）",
        g_winSize, g_winSize, g_restoreX, g_restoreY,
        coordPhys ? "是" : "否", coordPhys ? "" : " 已按 DPI 迁移");
}

// 切换状态机（右键菜单"切换场景"）：
// 【官方模式】各运行时文档（rive.app/docs/runtimes/web/state-machines reset()/
// cleanupInstances()、Flutter artboard.instance()、Android）一致做法 = 销毁旧的
// ArtboardInstance + SM 实例，从 File 重新创建全新 ArtboardInstance 再建 SM 实例。
// 同一 ArtboardInstance 上切换 SM 实例会残留前一 SM 的状态（动画实例/绘制内部
// 结构），导致切换后 draw 0 条（教训 16.14）。状态机必须配自己的 Artboard 实例。
static void switchStateMachine(size_t idx)
{
    if (!g_file || idx >= g_file->artboardCount()) return;
    // 用户指定过渡：切换瞬间先留旧画面快照（倒计时期间自然淡出，不播旧动画）
    if (g_hasRendered && g_dib_bits)
    {
        const UINT S = g_winSize;
        g_fadeShot.resize((size_t)S * S);
        memcpy(g_fadeShot.data(), g_dib_bits, (size_t)S * S * 4);
    }
    else
    {
        g_fadeShot.clear(); // 首次加载无旧画面（启动不播倒计时）
    }
    // §16.14：SM 切换必须连 ArtboardInstance 一起重建（官方各运行时一致做法）
    g_smi.reset();
    delete g_ab;
    g_ab = g_file->artboardDefault().release();
    if (!g_ab) { log("[FAIL] 重建 artboard"); g_fadeShot.clear(); return; }
    // 【16.27 官方做法】用 ArtboardInstance::stateMachineAt(idx) 创建实例（内部含
    // dataContext 继承 inheritDataContext，裸 new StateMachineInstance(sm, ab) 会漏掉
    // ——官方 player 即用 stateMachineAt/defaultStateMachine，对齐官方档案）。
    const rive::StateMachine* sm = g_ab->stateMachine(idx);
    g_smi = g_ab->stateMachineAt(idx);
    if (!g_smi) { log("[FAIL] stateMachineAt(%zu) 返回空", idx); g_fadeShot.clear(); return; }
    g_curSmIndex = (UINT)idx;
    bindInputs();
    savePetConfig(); // 场景切换即持久化（热启动恢复）
    // 进入交叉淡化过渡（有旧画面才播；启动首场景直接显示）：
    // Waiting=等待期放过场动画（DotsJump，用户定稿：无黑屏/无 rive 开场黑场）
    // → 新皮出画（rgbCnt>总像素2%，同启动裁剪）→ Crossfade 旧→新渐变
    if (!g_fadeShot.empty())
    {
        g_fadePhase = PetFadePhase::Waiting;
        g_fadePhaseStart = GetTickCount();
        log("[过渡] 场景切换 → 状态机[%zu] \"%s\"：等待新皮出画（过渡动画）→ 交叉淡化",
            idx, sm->name().c_str());
    }
    else
    {
        log("[OK] 切换场景 → 状态机[%zu] \"%s\" 输入=%zu 监听器=%zu",
            idx, sm->name().c_str(), sm->inputCount(), sm->listenerCount());
    }
}

// ---- 皮肤重载（上传/切换/恢复默认共用）：import 先验证，失败保持现状 ----
// path=新皮肤绝对路径；newSkin=新皮肤名（""=出厂默认）；
// resetSettings=true（恢复默认设置）时不读 config 恢复场景/设置，直接走默认策略。
static bool reloadSkinFromFile(const std::wstring& path, const std::string& newSkin,
                               bool resetSettings)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) { log("[皮肤] 重载失败: 无法打开 %s", wideToUtf8(path).c_str()); return false; }
    auto size = f.tellg();
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    f.close();
    rive::Span<const uint8_t> span(bytes.data(), bytes.size());
    rive::File* nf = rive::File::import(span, g_rc).release();
    if (!nf) { log("[皮肤] 重载失败: import 校验未通过（保持当前皮肤）"); return false; }
    // 旧画面快照（复用场景切换的淡出过渡）
    if (g_hasRendered && g_dib_bits)
    {
        const UINT S = g_winSize;
        g_fadeShot.resize((size_t)S * S);
        memcpy(g_fadeShot.data(), g_dib_bits, (size_t)S * S * 4);
    }
    else g_fadeShot.clear();
    g_smi.reset();
    delete g_ab;
    delete g_file; // 旧 File 释放；正在播放的音频源由 AudioEngine rcp 持有，安全
    g_file = nf;
    g_ab = g_file->artboardDefault().release();
    if (!g_ab) { log("[皮肤] 重载失败: artboardDefault（保持当前皮肤）"); g_fadeShot.clear(); return false; }
    log("[OK] 皮肤切换: artboard \"%s\" %.0fx%.0f 状态机=%zu 音频=%s",
        g_ab->name().c_str(), g_ab->width(), g_ab->height(),
        g_ab->stateMachineCount(), g_ab->hasAudio() ? "有" : "无");
    g_skinName = newSkin;
    // 默认场景策略：config 场景名（仅非重置时）> 状态机≥2 取 SM2（索引1，避开
    // boo2 SM1 空白坑）> 0；越界一律回退安全值
    size_t sm = g_ab->stateMachineCount() >= 2 ? 1 : 0;
    if (!resetSettings)
    {
        size_t initialSm = sm;
        loadPetConfig(&initialSm, g_ab->stateMachineCount());
        sm = initialSm;
    }
    switchStateMachine(sm);
    if (g_muted && g_engine) ma_engine_set_volume(g_engine->engine(), 0.0f);
    savePetConfig(); // skin 已更新，与场景/设置一并落盘（热启动恢复）
    log("[皮肤] 已生效: %s", g_skinName.empty() ? "出厂默认" : g_skinName.c_str());
    return true;
}

// 每帧更新输入（必须在 advance 之前调用）
static void updateInputs()
{
    // 打字监听消抖（用户指定节奏）：任意按键 → istyping=true + 重置 800ms；
    // 每帧递减空闲计时，停手 0.8s 自动回待机（严禁"按下=真/松开=假"直连）
    if (InterlockedExchange(&g_typingPulse, 0))
    {
        g_isTyping = true;
        g_idleMs = kTypingIdleMs;
        if (g_istypingInput) g_istypingInput->value(true);
        log("[输入] 打字 → istyping=true（空闲计时 %dms）", kTypingIdleMs);
    }
    if (g_isTyping)
    {
        g_idleMs -= PET_FPS_MS; // 每帧递减（33ms 帧）
        if (g_idleMs <= 0)
        {
            g_isTyping = false;
            if (g_istypingInput) g_istypingInput->value(false);
            log("[输入] 停手 0.8s → istyping=false（回待机）");
        }
    }
    // 眼睛跟随: 鼠标相对窗口中心位置归一化 -1..1
    if (g_mouseXInput || g_mouseYInput)
    {
        POINT cur;
        GetCursorPos(&cur);
        RECT rc;
        GetWindowRect(g_hwnd, &rc);
        float halfW = (float)(rc.right - rc.left) / 2.0f;
        float halfH = (float)(rc.bottom - rc.top) / 2.0f;
        float mx = halfW > 0 ? (float)(cur.x - (rc.left + rc.right) / 2) / halfW : 0;
        float my = halfH > 0 ? (float)(cur.y - (rc.top + rc.bottom) / 2) / halfH : 0;
        if (g_mouseXInput) g_mouseXInput->value(mx);
        if (g_mouseYInput) g_mouseYInput->value(my);
    }
    // 点击脉冲消抖（与 istyping 同款节奏）：任意左键按下（LL 鼠标钩子事件）→
    // click=true + 重置空闲计时；每帧递减，停手 kClickHoldMs 自动回 false。
    // 【click/drag 冲突修复】按住身体拖拽 = drag（抱住动画），不是 click。但点击脉冲
    // 来自全局鼠标钩子（任意左键按下），按身体拖拽同样触发 → 拖拽期间丢弃脉冲并
    // 强制 click=false，避免抱住与点击两动画同时播放。
    if (g_dragging)
    {
        InterlockedExchange(&g_clickPulse, 0);
        if (g_clickActive)
        {
            g_clickActive = false;
            if (g_clickInput) g_clickInput->value(false);
            log("[输入] 拖拽中抑制点击（click=false，避免与 drag 冲突）");
        }
    }
    else if (InterlockedExchange(&g_clickPulse, 0))
    {
        g_clickActive = true;
        g_clickIdleMs = kClickHoldMs;
        if (g_clickInput) g_clickInput->value(true);
        log("[输入] 点击 → click=true（保持 %dms）", kClickHoldMs);
    }
    if (g_clickActive && !g_dragging)
    {
        g_clickIdleMs -= PET_FPS_MS; // 每帧递减（33ms 帧）
        if (g_clickIdleMs <= 0)
        {
            g_clickActive = false;
            if (g_clickInput) g_clickInput->value(false);
            log("[输入] 点击保持结束 → click=false");
        }
    }
    // musicon（WASAPI 会话级"正在播放音乐"）：后台线程 500ms 去抖后写
    // g_musiconState（volatile），主循环每帧转发给契约输入。无句柄=皮肤未导出
    // musicon（契约缺失告警已由 bindInputs 打出）；播放状态翻转日志在检测线程。
    if (g_musiconInput)
    {
        const bool playing = InterlockedCompareExchange(&g_musiconState, 0, 0) != 0;
        g_musiconInput->value(playing);
    }
}

// ---- M3-3 按键显示 HUD：vk → 标签（复刻 Electron keyVkToLabel，按 Windows vkCode 对照） ----
static std::string vkToLabel(int vk)
{
    switch (vk)
    {
        case 8: return "Back"; case 9: return "Tab"; case 13: return "Enter";
        case 16: return "Shift"; case 17: return "Ctrl"; case 18: return "Alt";
        case 19: return "Pause"; case 20: return "Caps"; case 27: return "Esc";
        case 32: return "Space"; case 33: return "PageUp"; case 34: return "PageDown";
        case 35: return "End"; case 36: return "Home";
        case 37: return "\xe2\x86\x90"; case 38: return "\xe2\x86\x91"; // ← ↑
        case 39: return "\xe2\x86\x92"; case 40: return "\xe2\x86\x93"; // → ↓
        case 44: return "PrtSc"; case 45: return "Ins"; case 46: return "Del";
        case 91: case 92: return "Win"; case 93: return "Menu";
        case 96: return "Num0"; case 97: return "Num1"; case 98: return "Num2";
        case 99: return "Num3"; case 100: return "Num4"; case 101: return "Num5";
        case 102: return "Num6"; case 103: return "Num7"; case 104: return "Num8";
        case 105: return "Num9"; case 106: return "Num*"; case 107: return "Num+";
        case 109: return "Num-"; case 110: return "Num."; case 111: return "Num/";
        case 112: case 113: case 114: case 115: case 116: case 117: case 118:
        case 119: case 120: case 121: case 122: case 123:
            return "F" + std::to_string(vk - 111);
        case 160: case 161: return "Shift";  // 左/右 Shift（LL 钩子报 160/161 而非 16）
        case 162: case 163: return "Ctrl";   // 左/右 Ctrl
        case 164: case 165: return "Alt";    // 左/右 Alt
        case 144: return "NumLock"; case 145: return "ScrLk";
        case 186: return ";"; case 187: return "="; case 188: return ",";
        case 189: return "-"; case 190: return "."; case 191: return "/";
        case 192: return "`"; case 219: return "["; case 220: return "\\";
        case 221: return "]"; case 222: return "'";
    }
    if (vk >= 65 && vk <= 90) return std::string(1, (char)vk);
    if (vk >= 48 && vk <= 57) return std::string(1, (char)vk);
    return "Key" + std::to_string(vk);
}
static void hudKeyDown(int vk)
{
    const uint64_t now = GetTickCount64();
    EnterCriticalSection(&g_hudLock);
    HudKeyState& st = g_hudKeys[vkToLabel(vk)];
    st.held = true;
    st.down_ms = now;
    st.up_ms = 0;
    st.vk = vk;             // 记录原始 vk，供 drawKeyHud 物理真值兜底查询（§49）
    LeaveCriticalSection(&g_hudLock);
}
static void hudKeyUp(int vk)
{
    const uint64_t now = GetTickCount64();
    EnterCriticalSection(&g_hudLock);
    auto it = g_hudKeys.find(vkToLabel(vk));
    if (it != g_hudKeys.end())
    {
        it->second.held = false;
        it->second.up_ms = now;   // 触发延迟隐藏（渐变淡出）
    }
    LeaveCriticalSection(&g_hudLock);
}

// UTF-8 → UTF-16（GDI+ DrawString 需要宽字符）
static std::wstring utf8ToW(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// 画按键卡片（复刻 Electron #keyboard-hud，位置：窗口左下角）
// 【实现约束】不能把 Gdiplus::Bitmap 直接包 DIB 内存（32bppPARGB 语义会污染
// 整张位图 alpha → ULW 全透明，桌宠消失，教训见 §16.15）；改用 Graphics(HDC)
// 直接画到 DIB DC，卡片用不透明色（GDI 路径不支持真 alpha 混合，但 0.95→1.0
// 视觉无差），绝不触碰 rive 像素的 alpha 通道。
static void drawKeyHud(HDC dc, int W, int H)
{
    if (!g_gdipStarted) return;
    if (!g_hudEnabled)
    {
        // 关闭时仍清理过期键，避免 map 无限膨胀（下次打开状态干净）
        EnterCriticalSection(&g_hudLock);
        const uint64_t now = GetTickCount64();
        for (auto it = g_hudKeys.begin(); it != g_hudKeys.end();)
        {
            // 【§49】关闭分支同样按物理真值清理：物理已松开且 down 停止
            // ≥kHudKeyupLooseMs 视为 keyup 丢失直接擦除（不再等 3 秒超时），
            // 下次开启时 map 状态干净
            const bool phys_loose =
                it->second.held && it->second.vk > 0 &&
                !(GetAsyncKeyState(it->second.vk) & 0x8000) &&
                now - it->second.down_ms >= kHudKeyupLooseMs;
            const bool released_expired =
                !it->second.held && now - it->second.up_ms >= kHudHoldMs;
            const bool held_stale =
                it->second.held && now - it->second.down_ms >= kHudMaxHoldMs;
            if (released_expired || held_stale || phys_loose)
                it = g_hudKeys.erase(it);
            else
                ++it;
        }
        LeaveCriticalSection(&g_hudLock);
        return;
    }
    const float scale = (float)g_winSize / 400.0f; // Electron: scale = windowSize/400
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
    Gdiplus::Font font(L"Segoe UI", 26.0f * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt;
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    const int gap = (int)(8 * scale);
    const float pw = 18.0f * scale, ph = 10.0f * scale;
    const float rad = 12.0f * scale;

    // 可见卡片收集：按住 = 全亮；抬起但未过保持期 = 按 t(1→0) 渐变淡出；
    // 超过保持期 = 擦除（不渲染）。同键快速连击时 held 覆盖 up，视觉不闪。
    std::vector<std::pair<std::string, float>> cards;  // label + 亮度系数 t
    std::vector<std::string> expired;
    const uint64_t now = GetTickCount64();
    {
        EnterCriticalSection(&g_hudLock);
        for (auto& kv : g_hudKeys)
        {
            // 【根因 §49】keyup 丢失物理真值兜底：LL 钩子 keyup 偶发丢失（系统
            // 超时静默卸载钩子/失焦/UAC 安全桌面）→ held 永久常驻。每帧查
            // GetAsyncKeyState 物理真值：物理已松开且 down 停止 ≥kHudKeyupLooseMs
            // 即判定 keyup 丢失，转淡出流程；累计达 kHudKeyupLossRebuildAt 次
            // 重装 LL 钩子自愈（防复发）。真按住（物理按下）不受影响。
            if (kv.second.held && kv.second.vk > 0)
            {
                const bool physDown = (GetAsyncKeyState(kv.second.vk) & 0x8000) != 0;
                if (!physDown && now - kv.second.down_ms >= kHudKeyupLooseMs)
                {
                    kv.second.held = false;
                    kv.second.up_ms = now;
                    ++g_hudKeyupLossCount;
                    log("[HUD] keyup丢失兜底：%s 物理已松开但钩子未收up（累计%d次，达%d次重装钩子）",
                        kv.first.c_str(), g_hudKeyupLossCount, kHudKeyupLossRebuildAt);
                    if (g_hudKeyupLossCount >= kHudKeyupLossRebuildAt)
                    {
                        g_hudKeyupLossCount = 0;
                        rebuildKbdHook();   // 自愈：LL 钩子被系统卸载后重装，恢复 up 链路
                    }
                }
            }
            if (kv.second.held)
            {
                // 真按住（物理按下，含长按）→ 一直显示。kHudMaxHoldMs 仅作
                // GetAsyncKeyState 异常/不可用时的最终保险（防卡片永久常驻）
                if (now - kv.second.down_ms >= kHudMaxHoldMs)
                    expired.push_back(kv.first);
                else
                    cards.emplace_back(kv.first, 1.0f);
            }
            else
            {
                const uint64_t elapsed = now - kv.second.up_ms;
                if (elapsed < kHudHoldMs)
                {
                    const float t =
                        1.0f - (float)elapsed / (float)kHudHoldMs;  // 1→0
                    cards.emplace_back(kv.first, t);
                }
                else
                {
                    expired.push_back(kv.first);
                }
            }
        }
        for (const auto& label : expired) g_hudKeys.erase(label);
        LeaveCriticalSection(&g_hudLock);
    }

    // 左下角布局：卡片从窗口左侧起排，底部留 8px
    float x = 0;
    for (const auto& card : cards)
    {
        const std::string& label = card.first;
        const float t = card.second;  // 1=全亮 → 0=消失（文字/底色渐变模拟淡出）
        std::wstring wlabel = utf8ToW(label);
        Gdiplus::RectF lr(0, 0, 1000, 1000);
        g.MeasureString(wlabel.c_str(), -1, &font, lr, &fmt, &lr);
        const float w = lr.Width + pw * 2;
        const float h = 52.0f * scale;
        const float y = (float)H - h - 8.0f * scale;
        Gdiplus::GraphicsPath path;
        path.AddArc(x, y, rad, rad, 180.0f, 90.0f);
        path.AddArc(x + w - rad, y, rad, rad, 270.0f, 90.0f);
        path.AddArc(x + w - rad, y + h - rad, rad, rad, 0.0f, 90.0f);
        path.AddArc(x, y + h - rad, rad, rad, 90.0f, 90.0f);
        path.CloseFigure();
        // 底色：全亮 (20,20,20) → 淡出收敛到 (6,6,6)（不碰 alpha，§16.15）
        const BYTE bgv = (BYTE)((float)6 + ((float)20 - 6) * t);
        Gdiplus::SolidBrush bg(Gdiplus::Color(255, bgv, bgv, bgv));
        g.FillPath(&bg, &path);
        // 文字：全亮白 → 淡出收敛到近底色（视觉淡出）
        const BYTE fgv = (BYTE)((float)12 + ((float)255 - 12) * t);
        Gdiplus::SolidBrush fg(Gdiplus::Color(255, fgv, fgv, fgv));
        Gdiplus::RectF tr(x, y, w, h);
        g.DrawString(wlabel.c_str(), -1, &font, tr, &fmt, &fg);
        x += w + gap;
    }
}

// 窗口客户端坐标 -> artboard 坐标（从屏幕坐标换算）
static rive::Vec2D screenToArt(int scrX, int scrY)
{
    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    int wx = scrX - rc.left; // 窗口内像素（含窗口边框=0, 无边框窗口）
    int wy = scrY - rc.top;
    const UINT S = g_winSize;
    float artW = g_ab->width(), artH = g_ab->height();
    float scale = std::min((float)S / artW, (float)S / artH);
    float offX = ((float)S - artW * scale) / 2.0f;
    float offY = ((float)S - artH * scale) / 2.0f;
    return rive::Vec2D(((float)wx - offX) / scale, ((float)wy - offY) / scale);
}

// 喂指针事件给状态机（Hit Area 监听器驱动: 悬停鼻子→生气 / 点击奶茶→动画+音效 / 眼睛跟随）
static void petPointerMove(int scrX, int scrY)
{
    if (!g_smi) return;
    g_smi->pointerMove(screenToArt(scrX, scrY));
}
static void petPointerDown(int scrX, int scrY)
{
    if (!g_smi) return;
    g_smi->pointerDown(screenToArt(scrX, scrY));
}
static void petPointerUp(int scrX, int scrY)
{
    if (!g_smi) return;
    g_smi->pointerUp(screenToArt(scrX, scrY));
}
static void petPointerExit()
{
    if (!g_smi) return;
    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    g_smi->pointerExit(screenToArt(rc.right, rc.bottom)); // 任意点，Rive 内部按区域判定离开
}

// ---- 渲染资源管理（尺寸可变） ----
static void destroyRenderResources()
{
    if (g_dib_dc) { DeleteDC(g_dib_dc); g_dib_dc = nullptr; }
    if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_staging) { g_staging->Release(); g_staging = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    if (g_rt) { delete g_rt; g_rt = nullptr; }
    g_fadeShot.clear();
}

static bool createRenderResources(UINT size)
{
    // 【超采样 16.31】1.5x 渲染纹理（2x 基线 draw 160ms+ 不可用；1.5x 快帧 ~30ms 可接受，
    // 慢帧为动画复杂帧 1x 亦有）；readback 后 premult 双线性缩小到窗口尺寸
    const UINT R = size * 3 / 2;
    // 【闪烁 16.24】先建后删：全部新资源创建成功后才释放旧的。旧实现先 destroy
    // 再创建，重建空窗期窗口内容失效 → 滚轮缩放闪黑/残影。任一失败回滚保留旧资源。
    rive::gpu::RenderTargetD3D* newRt = nullptr;
    ID3D11Texture2D* newTex = nullptr;
    ID3D11Texture2D* newStaging = nullptr;
    ID3D11RenderTargetView* newRtv = nullptr;
    HBITMAP newDib = nullptr;
    HDC newDibDc = nullptr;
    BYTE* newDibBits = nullptr;

    newRt = static_cast<rive::gpu::RenderContextD3DImpl*>(g_rc->impl())
                ->makeRenderTarget(R, R).release();
    if (!newRt) { log("[FAIL] makeRenderTarget"); return false; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = R; td.Height = R; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc = {1, 0};
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &newTex))) { log("[FAIL] CreateTexture2D"); goto fail; }
    newRt->setTargetTexture(newTex);
    if (FAILED(g_dev->CreateRenderTargetView(newTex, nullptr, &newRtv))) { log("[FAIL] CreateRenderTargetView"); goto fail; }
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &newStaging))) { log("[FAIL] staging"); goto fail; }

    HDC hdc = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)size;
    bi.bmiHeader.biHeight = -(LONG)size; // top-down DIB
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    newDib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void**)&newDibBits, nullptr, 0);
    newDibDc = CreateCompatibleDC(hdc);
    ReleaseDC(nullptr, hdc);
    if (!newDib || !newDibDc || !newDibBits) { log("[FAIL] DIB"); goto fail; }
    SelectObject(newDibDc, newDib);

    // 全部创建成功 → 一次性替换旧资源（旧资源在替换前始终有效，无空窗）
    destroyRenderResources();
    g_rt = newRt;
    g_tex = newTex;
    g_rtv = newRtv;
    g_staging = newStaging;
    g_dib = newDib;
    g_dib_dc = newDibDc;
    g_dib_bits = newDibBits;
    return true;

fail:
    if (newRt) delete newRt;
    if (newTex) newTex->Release();
    if (newRtv) newRtv->Release();
    if (newStaging) newStaging->Release();
    if (newDibDc) DeleteDC(newDibDc);
    if (newDib) DeleteObject(newDib);
    log("[FAIL] 重建渲染资源失败（保持旧资源）");
    return false;
}

// 过渡期加载动画（定稿唯一方案：DotsJump 圆点跳动）：
// 四瓣白实心圆错峰上下弹跳（左上0/右上150/左下300/右下450ms 延迟，1.2s 一轮）+ 每瓣明暗呼吸（灰度 176..244）
// 布局统一（200 坐标系放大到窗口，整体缩小 50%）：上翅 r36 距中心 ±45,−32；下翅 r24 距中心 ±43,+28；上下相切、左右翼中间空隙 18px。
// 全部不透明色（§16.15：GDI+ 直画不透明，不污染 alpha；明暗用灰度变化模拟呼吸，不在透明底上用半透明）。
// 早期对比方案 FlapBreath（扇动呼吸，用户 2026-10-15 指定替换）已移除。
static void drawLoadingScene(HDC dc, int W, int H, DWORD elapsedMs)
{
    if (!g_gdipStarted) return;
    const float s = (float)W / 200.0f;      // 200 坐标系 → 窗口
    const float s2 = s * 0.5f;              // 整体缩小 50%（用户指定）
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float cx = (float)W / 2.0f, cy = (float)H / 2.0f;
    const float upR = 36.0f * s2, loR = 24.0f * s2;
    const float upOx = 45.0f * s2, upOy = 32.0f * s2;   // 上翅距中心
    const float loOx = 43.0f * s2, loOy = 28.0f * s2;   // 下翅距中心

    // DotsJump：错峰延迟 + 明暗呼吸（1.2s 一轮，空档 2.4s 跳两轮）
    const DWORD delay[4] = { 0, 150, 300, 450 };
    const float ox[4] = { -upOx, upOx, -loOx, loOx };
    const float oy[4] = { -upOy, -upOy, loOy, loOy };
    const float r[4]  = { upR, upR, loR, loR };
    for (int i = 0; i < 4; i++)
    {
        const float ph = (float)((elapsedMs + delay[i]) % 1200) / 1200.0f;
        const float j = 0.5f - 0.5f * cosf(ph * 6.2831853f);  // 0..1..0 平滑（ease-in-out）
        const float dy = -13.0f * s2 * j;                      // 上跳
        const int gv = (int)(244.0f * (0.72f + 0.28f * j));   // 明暗呼吸（白版灰度 176..244）
        Gdiplus::SolidBrush br(Gdiplus::Color(255, gv, gv + 2, gv + 6)); // 微天蓝白
        g.FillEllipse(&br, cx + ox[i] - r[i], cy + oy[i] + dy - r[i], r[i] * 2, r[i] * 2);
    }
}

// ---- 柔和阴影：从角色 alpha 提取软阴影，右下偏移，不覆盖角色像素 ----
// 性能：separable box blur（水平+垂直各一次），S=720 约 3ms；静态缓冲不每帧分配
// ---- 白边诊断：保存出画帧 PNG（raw=阴影前 / shadow=阴影后），本地直观对比 ----
// GDI+ PixelFormat32bppPARGB（预乘）字节序 = B,G,R,A，与 g_dib_bits DIB 完全一致，
// 直存不转换；Save 到 PNG 时 GDI+ 自动反预乘为 straight，显示与原屏一致。
static int getPngEncoderClsid(CLSID* clsid)
{
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    Gdiplus::ImageCodecInfo* infos =
        reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, infos);
    for (UINT i = 0; i < num; i++)
    {
        if (wcscmp(infos[i].MimeType, L"image/png") == 0)
        {
            *clsid = infos[i].Clsid;
            return 0;
        }
    }
    return -1;
}
static void diagSavePng(const wchar_t* name, const BYTE* bits, UINT S)
{
    if (!g_gdipStarted || bits == nullptr) return;
    CLSID pngClsid;
    if (getPngEncoderClsid(&pngClsid) != 0) return;
    Gdiplus::Bitmap bmp((INT)S, (INT)S, (INT)S * 4,
                        PixelFormat32bppPARGB, (BYTE*)bits);
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), name);
    if (bmp.Save(path, &pngClsid, nullptr) == Gdiplus::Ok)
        log("[诊断] 已保存 %ls", name);
}

// 【超采样验证探针 16.28】premult BGRA 双线性缩小（premult 空间插值，边缘无 halo）
static void diagDownscale(const BYTE* src, UINT sw, BYTE* dst, UINT dw)
{
    const float k = (float)sw / (float)dw;
    for (UINT y = 0; y < dw; y++)
    {
        const float sy = ((float)y + 0.5f) * k - 0.5f;
        const int y0 = sy < 0 ? 0 : (int)sy;
        const int y1 = (y0 + 1 >= (int)sw) ? y0 : y0 + 1;
        const float fy = sy - (float)y0;
        for (UINT x = 0; x < dw; x++)
        {
            const float sx = ((float)x + 0.5f) * k - 0.5f;
            const int x0 = sx < 0 ? 0 : (int)sx;
            const int x1 = (x0 + 1 >= (int)sw) ? x0 : x0 + 1;
            const float fx = sx - (float)x0;
            BYTE* d = dst + ((size_t)y * dw + x) * 4;
            for (int c = 0; c < 4; c++)
            {
                const float v00 = (float)src[(size_t)y0 * sw * 4 + (size_t)x0 * 4 + c];
                const float v10 = (float)src[(size_t)y0 * sw * 4 + (size_t)x1 * 4 + c];
                const float v01 = (float)src[(size_t)y1 * sw * 4 + (size_t)x0 * 4 + c];
                const float v11 = (float)src[(size_t)y1 * sw * 4 + (size_t)x1 * 4 + c];
                const float top = v00 + (v10 - v00) * fx;
                const float bot = v01 + (v11 - v01) * fx;
                float v = top + (bot - top) * fy;
                if (v < 0.0f) v = 0.0f;
                if (v > 255.0f) v = 255.0f;
                d[c] = (BYTE)(v + 0.5f);
            }
        }
    }
}

static void applySoftShadow(BYTE* bits, UINT S)
{
    static std::vector<uint8_t> s_alpha, s_tmp;
    const size_t n = (size_t)S * S;
    if (s_alpha.size() != n) { s_alpha.assign(n, 0); s_tmp.assign(n, 0); }

    const int dx = 4, dy = 6;       // 右下偏移
    const int radius = 3;           // box blur 半径
    const int shadowA = 90;        // 最大不透明度 ~35%
    const uint8_t shR = 35, shG = 35, shB = 35;

    // 1. 提取角色 alpha（bits 是 BGRA 序，[3]=A）
    for (size_t i = 0; i < n; i++)
        s_alpha[i] = bits[i * 4 + 3];

    // 2. separable box blur：先水平，再垂直（两次 1D 遍历 = 近似高斯）
    for (int y = 0; y < (int)S; y++)
    {
        for (int x = 0; x < (int)S; x++)
        {
            int sum = 0, cnt = 0;
            for (int k = -radius; k <= radius; k++)
            {
                int nx = x + k;
                if (nx >= 0 && nx < (int)S) { sum += s_alpha[y * S + nx]; cnt++; }
            }
            s_tmp[y * S + x] = (uint8_t)(sum / cnt);
        }
    }
    for (int y = 0; y < (int)S; y++)
    {
        for (int x = 0; x < (int)S; x++)
        {
            int sum = 0, cnt = 0;
            for (int k = -radius; k <= radius; k++)
            {
                int ny = y + k;
                if (ny >= 0 && ny < (int)S) { sum += s_tmp[ny * S + x]; cnt++; }
            }
            s_alpha[y * S + x] = (uint8_t)(sum / cnt);
        }
    }

    // 3. 分层阴影写入（对齐 Electron 双 canvas 语义：阴影层在角色层之下）
    //    - 透明区(curA<8)：直接写阴影（premult）
    //    - 角色半透明边缘(8≤curA<255)：分层合成——角色在上、阴影在下，
    //      角色半透明处"透出"下方阴影：newRGB = 角色RGB + 阴影RGB*(1-curA/255)，
    //      newA = curA + 阴影A*(1-curA/255)。数学等价"先画阴影层、再叠角色层"。
    //    - 不透明(curA≥255)：阴影被完全遮挡，跳过。
    for (int y = 0; y < (int)S; y++)
    {
        for (int x = 0; x < (int)S; x++)
        {
            size_t idx = (size_t)(y * S + x) * 4;
            BYTE curA = bits[idx + 3];
            int sx = x - dx, sy = y - dy;
            if (sx < 0 || sy < 0 || sx >= (int)S || sy >= (int)S) continue;
            uint8_t sa = s_alpha[sy * S + sx];
            if (sa < 20) continue;  // 稳定值：降到 6 跟随动画会卡顿且效果不明显，回退
            int a = sa * shadowA / 255;
            if (a < 8) continue;
            if (curA < 8)
            {
                // 透明区：直接写阴影（premult RGB）
                BYTE pa = (BYTE)(shR * a / 255);
                bits[idx + 0] = pa;  // B
                bits[idx + 1] = pa;  // G
                bits[idx + 2] = pa;  // R
                bits[idx + 3] = (BYTE)a;
            }
            else if (curA < 255)
            {
                // 分层合成：角色在上、阴影在下（半透明处透出阴影）
                const int invE = 255 - curA;
                const int sRGB = shR * a / 255;  // 阴影 premult RGB
                int nb = bits[idx + 0] + (sRGB * invE) / 255;
                int ng = bits[idx + 1] + (sRGB * invE) / 255;
                int nr = bits[idx + 2] + (sRGB * invE) / 255;
                int na = curA + (a * invE) / 255;
                if (nb > 255) nb = 255;
                if (ng > 255) ng = 255;
                if (nr > 255) nr = 255;
                if (na > 255) na = 255;
                bits[idx + 0] = (BYTE)nb;
                bits[idx + 1] = (BYTE)ng;
                bits[idx + 2] = (BYTE)nr;
                bits[idx + 3] = (BYTE)na;
            }
            // curA>=255：阴影不可见，跳过
        }
    }
}

// ---- 渲染一帧: advance -> draw -> 读回 -> ULW 合成 ----
static void renderFrame()
{
    const UINT S = g_winSize;
    const UINT R = S * 3 / 2;   // 【超采样 16.31】1.5x 超采样（2x 在 rive D3D11 PLS 1024² 基线 draw
                                //  160ms+ 不可用；1.5x 快帧 ~30ms 可接受，慢帧为动画复杂帧（1x 亦有））
    // §52 帧耗时诊断：分段计时，仅超阈值打日志（定位悬停/菜单卡顿瓶颈，避免日志洪泛）
    static LARGE_INTEGER perfFreq{};
    if (!perfFreq.QuadPart) QueryPerformanceFrequency(&perfFreq);
    LARGE_INTEGER t0{}, t1{}, t2{}, t3{}, t4{};
    QueryPerformanceCounter(&t0);
    // 启动加载中（riv 未就绪）：透明底 + DotsJump 加载动画（复用切换场景的
    // 过渡过场——用户指定"中间那个圆点浮动的过场直接复用到启动开场"）
    if (!g_ab || !g_smi || !g_tex)
    {
        if (!g_dib_bits) return;
        memset(g_dib_bits, 0, (size_t)S * S * 4);
        drawLoadingScene(g_dib_dc, (int)S, (int)S, GetTickCount() - g_loadingStart);
        POINT ptSrc{0, 0};
        SIZE sz{(LONG)S, (LONG)S};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(g_hwnd, nullptr, nullptr, &sz, g_dib_dc, &ptSrc, 0, &bf, ULW_ALPHA);
        return;
    }
    updateInputs(); // 每帧先更新输入（mouse/悬停复位），再推进状态机
    // 必须用 advanceAndApply：内部 = advance + artboard advanceInternal(Animate)
    // + tryChangeState 循环 + updatePass；两段式 advance()+artboard->advance()
    // 不会完整应用动画 pose（boo2 SM1 曾因此渲染全空，教训 16.13）
    g_smi->advanceAndApply(0.033f);
    QueryPerformanceCounter(&t1);   // §52 advance 完成

    // 每帧先清零渲染纹理（rive 的 PLS flush 只写 draw 覆盖的像素，未覆盖区域残留垃圾数据）
    // 【诊断 16.14】ClearRenderTargetView + PLS loadAction=clear 双重清除疑致切换后渲染全空：
    // 冒烟(无 ClearRenderTargetView)切换 PASS，实机(有)切换必现 0 像素 → 移除手动清除，
    // 交给 PLS 的 loadAction=clear（实验验证后定论）
    // const FLOAT clearColor[4] = {0, 0, 0, 0};
    // g_ctx->ClearRenderTargetView(g_rtv, clearColor);

    rive::gpu::RenderContext::FrameDescriptor fd{};
    fd.renderTargetWidth = R;
    fd.renderTargetHeight = R;
    fd.loadAction = rive::gpu::LoadAction::clear;
    fd.clearColor = 0x00000000;
    g_rc->beginFrame(fd);

    rive::RiveRenderer renderer(g_rc);
    renderer.save();
    renderer.align(rive::Fit::contain, rive::Alignment::center,
                   rive::AABB(0, 0, (float)R, (float)R),
                   rive::AABB(0, 0, g_ab->width(), g_ab->height()));
    g_ab->draw(&renderer);
    renderer.restore();

    rive::gpu::RenderContext::FlushResources fr{};
    fr.renderTarget = g_rt;
    fr.currentFrameNumber = ++g_frameNumber; // 官方惯例：每帧递增
    g_rc->flush(fr);
    g_ctx->Flush();
    QueryPerformanceCounter(&t2);   // §52 draw+flush 完成

    // 读回 staging
    g_ctx->CopyResource(g_staging, g_tex);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    // 【回退 §52】DO_NOT_WAIT 与异步 CopyResource 不匹配：copy 刚发起即 Map 必返回
    // WAS_STILL_DRAWING → 每帧失败跳过读回 → 画面永久冻结（"加载卡死"）。恢复阻塞 Map。
    if (FAILED(g_ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return;
    // D3D11 R8G8B8A8 内存字节序 = R,G,B,A；DIB 32bpp (BI_RGB) 期望 B,G,R,A → 拷贝时交换 R/B
    const BYTE* src = static_cast<const BYTE*>(mapped.pData);
    // 【白边诊断探针 16.27】统计边缘半透明像素(8≤A<255)的 premult 一致性：
    // premult 输出要求每个 RGB 通道 ≤ A；若 readback 出现 RGB>A（straight 成分），
    // ULW AC_SRC_ALPHA 按 premult 合成会把边缘合成得过亮 = 白边。只读统计不改像素。
    UINT64 edgeCnt = 0, edgeBadCnt = 0;
    UINT64 badSx[4] = {}, badSy[4] = {};
    int badR[4] = {}, badG[4] = {}, badB[4] = {}, badA[4] = {};
    UINT badN = 0;
    // 探针3：透明区(A<8)"亮 RGB"统计（白边=轮廓外透明区发白）
    UINT64 haloRawCnt = 0;
    UINT64 hSx[4] = {}, hSy[4] = {};
    int hR[4] = {}, hG[4] = {}, hB[4] = {}, hA[4] = {};
    UINT hN = 0;
    UINT64 opaqueCnt = 0;
    UINT64 alphaCnt = 0;    // alpha>128
    UINT64 rgbCnt = 0;      // RGB 总和>30（不看 alpha）
    double alphaSum = 0.0, rgbSum = 0.0;

    // 【超采样 16.31】恒定 2x 高分 readback（正常/诊断共用）：渲染纹理 R=2S，本段
    // 每帧把 readback 像素按 BGRA 序拷入 hiresBuf，再 premult 双线性缩小到窗口尺寸。
    // 低分辨率渲染（无 MSAA）是锯齿+白边观感唯一根因；2x 缩小后边缘平滑过渡，
    // 对齐 Electron Canvas2D AA 质量。阴影仍在显示空间（S）做，位置/浓淡不变。
    static std::vector<BYTE> hiresBuf;
    if (hiresBuf.size() < (size_t)R * R * 4) hiresBuf.assign((size_t)R * R * 4, 0);
    UINT64 hEdge = 0, hBad = 0, hHalo = 0, hOpaque = 0, hRgb = 0;
    for (UINT y = 0; y < R; y++)
    {
        const BYTE* srow = src + (size_t)y * mapped.RowPitch;
        BYTE* drow = hiresBuf.data() + (size_t)y * R * 4;
        for (UINT x = 0; x < R; x++)
        {
            drow[x * 4 + 0] = srow[x * 4 + 2]; // B
            drow[x * 4 + 1] = srow[x * 4 + 1]; // G
            drow[x * 4 + 2] = srow[x * 4 + 0]; // R
            drow[x * 4 + 3] = srow[x * 4 + 3]; // A
            if (g_diagEdge)
            {
                const int a = srow[x * 4 + 3];
                const int r = srow[x * 4 + 0], g_ = srow[x * 4 + 1], b = srow[x * 4 + 2];
                if (a > 30 || r + g_ + b > 90) hOpaque++;
                if (r + g_ + b > 30) hRgb++;
                if (a > 8 && a < 255)
                {
                    hEdge++;
                    const int mx = r > g_ ? (r > b ? r : b) : (g_ > b ? g_ : b);
                    if (mx > a + 2) hBad++;
                }
                else if (a < 8)
                {
                    const int mx = r > g_ ? (r > b ? r : b) : (g_ > b ? g_ : b);
                    if (mx > 30) hHalo++;
                }
            }
        }
    }
    g_ctx->Unmap(g_staging, 0);
    // 诊断：出画首帧保存高分原图 + 打印高分统计
    if (g_diagEdge)
    {
        if (!g_hasShown && hRgb > (UINT64)((double)R * R * 0.02))
        {
            g_hasShown = true;
            log("[高分诊断] 首帧出画 R=%u", R);
        }
        if (!g_diagShotRaw && g_hasShown)
        {
            diagSavePng(L"_diag_raw_hires.png", hiresBuf.data(), R);
            g_diagShotRaw = true;
        }
        log("[高分诊断] R=%u edge=%llu bad=%llu halo=%llu opaque=%llu rgb=%llu",
            R, (unsigned long long)hEdge, (unsigned long long)hBad,
            (unsigned long long)hHalo, (unsigned long long)hOpaque, (unsigned long long)hRgb);
    }
    // premult 双线性缩小到窗口尺寸（显示空间；后续阴影/ULW 均在 S 空间）
    diagDownscale(hiresBuf.data(), R, g_dib_bits, S);
    {
        const BYTE* sp = g_dib_bits;
        for (UINT y = 0; y < S; y++)
        {
            for (UINT x = 0; x < S; x++)
            {
                const BYTE bv = sp[0], gv = sp[1], rv = sp[2], av = sp[3];
                sp += 4;
                alphaSum += av;
                rgbSum += rv + gv + bv;
                if (av > 30 || rv + gv + bv > 90)
                    opaqueCnt++;
                if (av > 128)
                    alphaCnt++;
                if (rv + gv + bv > 30)
                    rgbCnt++;
                // 探针：边缘半透明像素 premult 检查（RGB≤A 才符合 premult）
                if (av > 8 && av < 255)
                {
                    edgeCnt++;
                    const int mx = rv > gv ? (rv > bv ? rv : bv) : (gv > bv ? gv : bv);
                    if (mx > av + 2)
                    {
                        edgeBadCnt++;
                        if (badN < 4 && rv + gv + bv > 60)
                        {
                            badSx[badN] = x;
                            badSy[badN] = y;
                            badR[badN] = rv;
                            badG[badN] = gv;
                            badB[badN] = bv;
                            badA[badN] = av;
                            badN++;
                        }
                    }
                }
                else if (av < 8)
                {
                    // 透明区亮像素：轮廓外发白 = 白边直接来源
                    const int mx = rv > gv ? (rv > bv ? rv : bv) : (gv > bv ? gv : bv);
                    if (mx > 30)
                    {
                        haloRawCnt++;
                        if (hN < 4 && mx > 60)
                        {
                            hSx[hN] = x; hSy[hN] = y;
                            hR[hN] = rv; hG[hN] = gv; hB[hN] = bv; hA[hN] = av;
                            hN++;
                        }
                    }
                }
            }
        }
    }
    QueryPerformanceCounter(&t3);   // §52 readback+缩小+统计 完成
    // 白边诊断：出画稳定帧保存"阴影前"PNG（纯 rive readback 像素）
    // 已由 _diag_raw_hires.png（高分原图）覆盖，此处不再单独保存
    // 白边诊断打印（仅 --diag-edge 模式，节流避免日志洪泛）
    if (g_diagEdge)
    {
        static UINT s_diagPrints = 0;
        if (++s_diagPrints <= 3 || s_diagPrints % 15 == 0)
        {
            log("[白边诊断] edge=%llu bad=%llu (%.2f%%) halo_raw=%llu 样本:",
                (unsigned long long)edgeCnt, (unsigned long long)edgeBadCnt,
                edgeCnt ? (double)edgeBadCnt * 100.0 / (double)edgeCnt : 0.0,
                (unsigned long long)haloRawCnt);
            for (UINT i = 0; i < badN; i++)
            {
                int mx = badR[i] > badG[i] ? (badR[i] > badB[i] ? badR[i] : badB[i])
                                           : (badG[i] > badB[i] ? badG[i] : badB[i]);
                log("  (%llu,%llu) R=%d G=%d B=%d A=%d 超A=%d",
                    (unsigned long long)badSx[i], (unsigned long long)badSy[i],
                    badR[i], badG[i], badB[i], badA[i], mx - badA[i]);
            }
            for (UINT i = 0; i < hN; i++)
                log("  [透明区亮] (%llu,%llu) R=%d G=%d B=%d A=%d",
                    (unsigned long long)hSx[i], (unsigned long long)hSy[i],
                    hR[i], hG[i], hB[i], hA[i]);
        }
    }
    g_hasRendered = true; // 首帧渲染完成（供切换淡化判断）

    // ---- 场景切换交叉淡化过渡（黑屏根因修复：全程无透明间隙、无 rive 开场黑场） ----
    // Waiting：新皮未出画 → 显示过渡加载动画（DotsJump，透明底，无黑屏/无 rive 黑场）；
    //           出画判定 = 启动裁剪 g_hasShown 同款（rgbCnt > 总像素 2%，自适应任意
    //           riv 过场时长，不写死时间）；未出画就一直等待（用户定稿），无超时强制。
    // Crossfade：出画后旧快照→新帧 400ms 渐变（用户确认保留）。
    if (g_fadePhase != PetFadePhase::None)
    {
        uint32_t* dst = reinterpret_cast<uint32_t*>(g_dib_bits);
        if (g_fadePhase == PetFadePhase::Waiting)
        {
            // 【用户定稿】等待期放过场动画（DotsJump）：先清透明再画（§16.18 教训，
            // 本帧 readback 可能是 rive 开场"不透明黑帧"，不清会把黑底播出去）
            memset(g_dib_bits, 0, (size_t)S * S * 4);
            drawLoadingScene(g_dib_dc, (int)S, (int)S, GetTickCount() - g_loadingStart);
            // 出画判定：与启动 g_hasShown 完全同款（RGB 内容像素 > 总像素 2%）
            if (rgbCnt > (UINT64)((double)S * S * 0.02))
            {
                g_fadePhase = PetFadePhase::Crossfade;
                g_fadePhaseStart = GetTickCount();
                log("[过渡] 新场景出画（rgb=%llu），开始旧→新交叉淡化",
                    (unsigned long long)rgbCnt);
            }
        }
        else // Crossfade：旧快照→新帧 渐变（ease-out，开始慢结尾快）
        {
            const DWORD el = GetTickCount() - g_fadePhaseStart;
            float p = (float)el / (float)kFadeInMs;
            if (p > 1.0f) p = 1.0f;
            if (p >= 1.0f)
            {
                g_fadePhase = PetFadePhase::None;
                g_fadeShot.clear();
                log("[过渡] 过渡完成");
            }
            else if (g_fadeShot.size() == (size_t)S * S)
            {
                const float inv = 1.0f - p;
                for (size_t i = 0; i < (size_t)S * S; i++)
                {
                    const uint32_t o = g_fadeShot[i];
                    const uint32_t n = dst[i];
                    dst[i] = (uint32_t)(((o & 0xFF) * inv + (n & 0xFF) * p)) |
                             ((uint32_t)((((o >> 8) & 0xFF) * inv + ((n >> 8) & 0xFF) * p)) << 8) |
                             ((uint32_t)((((o >> 16) & 0xFF) * inv + ((n >> 16) & 0xFF) * p)) << 16) |
                             ((uint32_t)((((o >> 24) & 0xFF) * inv + ((n >> 24) & 0xFF) * p)) << 24);
                }
            }
        }
    }

    // 渲染诊断（每 30 帧输出一次非空像素，判断渲染是否透明）
    if (++g_diagFrames >= 30)
    {
        const double tot = (double)S * S;
        log("[渲染] 非空 %llu (%.1f%%) | alpha>128 %llu (%.1f%%) | RGB>30 %llu (%.1f%%) | 均alpha %.0f 均RGB %.0f %s",
            (unsigned long long)opaqueCnt, opaqueCnt * 100.0 / tot,
            (unsigned long long)alphaCnt, alphaCnt * 100.0 / tot,
            (unsigned long long)rgbCnt, rgbCnt * 100.0 / tot,
            alphaSum / tot, rgbSum / tot,
            opaqueCnt > 1000 ? "有内容" : "全空!");
        // 【诊断 16.14】drawable 状态 dump：判别"切换后全空"是 artboard 组件
        // 被折叠/隐藏（状态机动画副作用）还是 renderer 未绘制
        {
            const auto& drs = g_ab->drawables();
            int collapsed = 0, hidden = 0;
            for (auto* d : drs)
            {
                if (!d) continue;
                if (d->isCollapsed()) collapsed++;
                if (d->isHidden()) hidden++;
            }
            log("[诊断] drawable=%zu collapsed=%d hidden=%d SM[%u]",
                drs.size(), collapsed, hidden, g_curSmIndex);
        }
        g_diagFrames = 0;
    }

    // ULW 合成
    // 【根因 16.17】SM1 动画自带"开场黑场"（唤醒流程，JS runtime 对照同样有：
    // 0.3s 深灰黑→1.5s 才出人物）。直接显示会把黑场播给用户。
    // 修复：首次"出画"（rgbCnt>总像素 2%）才显示；5s 超时兜底强制显示防永久透明。
    // 【防漂移】0.4% 口径曾漂移为固定 1000px（720²=0.19%），SM 开场 RIVE 黑帧
    // （1666px=0.32%@720²）漏过→启动黑屏；现实现与注释统一为总像素 2%（自适应窗口，
    // RIVE 0.32% 被拦、郭嘉/蝴蝶出画 20%+ 秒过），蝴蝶过场保持到真正出画。
    // 超时基准 = 加载完成时刻 g_loadDoneTick（加载本身再慢也不触发误超时）。
    // 【根因 16.19】g_loadDoneTick 初始 0——加载线程执行到一半（switchStateMachine
    // 完成、音频未完成）主线程进正常分支时 GetTickCount()-0 = 巨数 → 立即误判超时
    // → 直接把 SM 开场"不透明纯黑帧"（alpha=255 RGB=0）播给用户 → 启动黑屏 1~2s。
    // 修复：超时判定必须等 g_loadOk=true 之后才生效。
    bool showNow = g_hasShown;
    if (!g_hasShown)
    {
        if (rgbCnt > (UINT64)((double)S * S * 0.02) || (g_loadOk && (GetTickCount() - g_loadDoneTick) > 5000))
        {
            g_hasShown = true;
            showNow = true;
            log("[显示] 首帧出画，开始显示桌宠");
        }
    }
    // 输入法切换不隐藏桌宠（用户 2026-10-14 定稿：切走只是按键联动自然失效，
    // 桌宠保持显示；用户要关自己右键退出）。此处不再做 alpha 淡化/隐藏。
    POINT ptSrc{0, 0};
    SIZE sz{(LONG)S, (LONG)S};
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    BOOL ulwOk = false;
    if (showNow)
    {
        // 【修复 DotsJump 白边】Waiting 状态下是过场加载动画（DotsJump），
        // 不需要角色阴影和 HUD 卡片。applySoftShadow 会提取 DotsJump 圆点的 alpha
        // 写阴影，叠加在圆点透明区让边缘更亮 → 白边。
        if (g_fadePhase != PetFadePhase::Waiting)
        {
            applySoftShadow(g_dib_bits, S);  // 柔和阴影（右下偏移 4,6，预乘 alpha 修复白圈）
            // 按键显示 HUD 卡片（GDI+ 叠加到 ULW 位图，复刻 Electron #keyboard-hud）
            drawKeyHud(g_dib_dc, (int)S, (int)S);
            // 白边诊断：保存"阴影后"PNG（含阴影，代表实际 ULW 内容）
            if (g_diagEdge && !g_diagShotShadow && g_hasShown &&
                g_fadePhase == PetFadePhase::None)
            {
                diagSavePng(L"_diag_shadow.png", g_dib_bits, S);
                g_diagShotShadow = true;
            }
            // 【白边诊断探针2 16.27】applySoftShadow 后重扫：阴影是否污染边缘像素、
            // 阴影自身是否预乘正确（三通道相等深灰且 RGB≤A）。只读，不改像素。
            if (g_diagEdge)
            {
                static UINT s_p2 = 0;
                if (++s_p2 <= 3 || s_p2 % 30 == 0)
                {
                    UINT64 edgeCnt2 = 0, edgeBad2 = 0, shadowCnt = 0;
                    UINT64 haloShadowCnt = 0;
                    UINT64 hsSx[4] = {}, hsSy[4] = {};
                    int hsR[4] = {}, hsG[4] = {}, hsB[4] = {}, hsA[4] = {};
                    UINT hsN = 0;
                    UINT64 ebSx[4] = {}, ebSy[4] = {};
                    int ebR[4] = {}, ebG[4] = {}, ebB[4] = {}, ebA[4] = {};
                    UINT ebN = 0;
                    UINT64 shSx[4] = {}, shSy[4] = {};
                    int shR[4] = {}, shG[4] = {}, shB[4] = {}, shA[4] = {};
                    UINT shN = 0;
                    for (UINT yy = 0; yy < S; yy++)
                    {
                        const BYTE* row = g_dib_bits + (size_t)yy * S * 4;
                        for (UINT xx = 0; xx < S; xx++)
                        {
                            const BYTE bv = row[xx * 4 + 0], gv = row[xx * 4 + 1],
                                      rv = row[xx * 4 + 2], av = row[xx * 4 + 3];
                            // 先判阴影（预乘深灰：三通道相等且 ≤20），再判边缘
                            const bool isShadow = (bv == gv && gv == rv && rv <= 20);
                            if (isShadow && av >= 8)
                            {
                                shadowCnt++;
                                if (shN < 4)
                                {
                                    shSx[shN] = xx; shSy[shN] = yy;
                                    shR[shN] = rv; shG[shN] = gv;
                                    shB[shN] = bv; shA[shN] = av;
                                    shN++;
                                }
                            }
                            else if (av > 8 && av < 255)
                            {
                                edgeCnt2++;
                                const int mx = rv > gv ? (rv > bv ? rv : bv)
                                                       : (gv > bv ? gv : bv);
                                if (mx > av + 2)
                                {
                                    edgeBad2++;
                                    if (ebN < 4 && rv + gv + bv > 60)
                                    {
                                        ebSx[ebN] = xx; ebSy[ebN] = yy;
                                        ebR[ebN] = rv; ebG[ebN] = gv;
                                        ebB[ebN] = bv; ebA[ebN] = av;
                                        ebN++;
                                    }
                                }
                            }
                            else if (av < 8)
                            {
                                // 阴影后透明区亮像素（白边直接来源）
                                const int mx = rv > gv ? (rv > bv ? rv : bv)
                                                       : (gv > bv ? gv : bv);
                                if (mx > 30)
                                {
                                    haloShadowCnt++;
                                    if (hsN < 4 && mx > 60)
                                    {
                                        hsSx[hsN] = xx; hsSy[hsN] = yy;
                                        hsR[hsN] = rv; hsG[hsN] = gv;
                                        hsB[hsN] = bv; hsA[hsN] = av;
                                        hsN++;
                                    }
                                }
                            }
                        }
                    }
                    log("[白边诊断2] edge=%llu bad=%llu (%.2f%%) shadow=%llu halo_shadow=%llu",
                        (unsigned long long)edgeCnt2, (unsigned long long)edgeBad2,
                        edgeCnt2 ? (double)edgeBad2 * 100.0 / (double)edgeCnt2 : 0.0,
                        (unsigned long long)shadowCnt,
                        (unsigned long long)haloShadowCnt);
                    for (UINT i = 0; i < ebN; i++)
                        log("  [坏边] (%llu,%llu) R=%d G=%d B=%d A=%d",
                            (unsigned long long)ebSx[i], (unsigned long long)ebSy[i],
                            ebR[i], ebG[i], ebB[i], ebA[i]);
                    for (UINT i = 0; i < shN; i++)
                        log("  [阴影] (%llu,%llu) R=%d G=%d B=%d A=%d",
                            (unsigned long long)shSx[i], (unsigned long long)shSy[i],
                            shR[i], shG[i], shB[i], shA[i]);
                    for (UINT i = 0; i < hsN; i++)
                        log("  [透明区亮] (%llu,%llu) R=%d G=%d B=%d A=%d",
                            (unsigned long long)hsSx[i], (unsigned long long)hsSy[i],
                            hsR[i], hsG[i], hsB[i], hsA[i]);
                }
            }
        }
        // 过渡期加载动画（DotsJump 圆点跳动，画在透明底上）
        if (g_loadingDraw)
        {
            drawLoadingScene(g_dib_dc, (int)S, (int)S, GetTickCount() - g_loadingStart);
            if (++g_ldDiagFrames >= 30) { g_ldDiagFrames = 0; log("[加载] 过渡加载动画绘制中"); }
        }
        ulwOk = UpdateLayeredWindow(g_hwnd, nullptr, nullptr, &sz, g_dib_dc, &ptSrc, 0, &bf, ULW_ALPHA);
    }
    else
    {
        // 启动等待出画（SM 开场黑场不播给用户）：持续显示加载动画，出画帧无缝衔接
        // 【根因 16.18】必须先清透明再画过场——本帧 g_dib_bits 是 rive 首帧
        // "不透明黑帧"的 readback，直接画过场会把黑底一起播出去（用户报启动黑屏；
        // 与 FadeIn 等出画分支先 memset 同一教训，此处曾漏掉）
        memset(g_dib_bits, 0, (size_t)S * S * 4);
        drawLoadingScene(g_dib_dc, (int)S, (int)S, GetTickCount() - g_loadingStart);
        ulwOk = UpdateLayeredWindow(g_hwnd, nullptr, nullptr, &sz, g_dib_dc, &ptSrc, 0, &bf, ULW_ALPHA);
    }
    if (!ulwOk && !g_ulwWarned && g_hasShown)
    {
        g_ulwWarned = true;
        log("[渲染] ULW 合成失败 GLE=%lu", (unsigned long)GetLastError());
    }
    // §52 帧耗时诊断（阈值 20ms：30fps 帧预算 33ms，超过即明显掉帧）
    QueryPerformanceCounter(&t4);
    const double msTotal = (double)(t4.QuadPart - t0.QuadPart) * 1000.0 / (double)perfFreq.QuadPart;
    if (msTotal > 20.0)
    {
        const double msA = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)perfFreq.QuadPart;
        const double msD = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)perfFreq.QuadPart;
        const double msR = (double)(t3.QuadPart - t2.QuadPart) * 1000.0 / (double)perfFreq.QuadPart;
        const double msU = (double)(t4.QuadPart - t3.QuadPart) * 1000.0 / (double)perfFreq.QuadPart;
        log("[性能] 帧 %.1fms (>20ms): advance=%.1f draw=%.1f readback=%.1f ulw=%.1f 悬停=%d 菜单=%d",
            msTotal, msA, msD, msR, msU, g_hover ? 1 : 0, g_openMenu ? 1 : 0);
    }
}

// ---- 缩放（右下角锚定）——两拍冻结（仿拖拽 16.29 机制） ----
// 第一拍：换资源 + 移动位置，不渲染（内容冻结）；第二拍（下一拍 WM_TIMER）由
// renderFrame 的 ULW 一次性改大小+内容。SetWindowPos 移动与 ULW 贴位图严格
// 不同帧——与"拖动中冻结、松手下一拍渲染"同一机制，避开同帧竞争闪烁。
static void resizePetPhase1(UINT newSize)
{
    newSize = std::max<UINT>((UINT)PET_MIN_SIZE, std::min<UINT>((UINT)PET_MAX_SIZE, newSize));
    if (newSize == g_winSize) return;
    // 先建后删（16.24）在 createRenderResources 内部；成功后才更新 g_winSize，
    // 失败保持旧资源+旧尺寸（旧实现先改 g_winSize 再建资源，失败后新旧不一致
    // 会让 renderFrame 用新尺寸写旧 DIB → 越界）。
    if (!createRenderResources(newSize)) { log("[FAIL] 重建渲染资源失败"); return; }
    RECT rc{};
    GetWindowRect(g_hwnd, &rc);
    int right = rc.right, bottom = rc.bottom;
    g_winSize = newSize;
    g_resizeAppliedTarget = newSize;
    // 【闪烁 16.24】位置先行（SWP_NOSIZE），大小交给第二拍 renderFrame 的 ULW
    // psize 一次性更新——避免 SetWindowPos 改大小后旧尺寸位图被系统拉伸/复制的
    // 中间态；SWP_NOCOPYBITS 禁止系统复制旧位图（官方防分层窗口闪烁做法）。
    // 本拍不调 renderFrame → 内容冻结，与拖拽"移动中不贴位图"同一机制。
    SetWindowPos(g_hwnd, nullptr, right - (int)newSize, bottom - (int)newSize,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    log("[OK] 缩放第一拍: 位置就位 %ux%u（内容冻结，下一拍渲染）", newSize, newSize);
}

// ---- 鼠标穿透切换（悬停可交互 / 离开穿透桌面） ----
// §52 悬停判定收窄到"身体像素"：桌宠窗口是方形、身体圆形/不规则，旧逻辑用整个
// 窗口矩形判定 → 光标刚进透明边角就触发皮肤翻转/交互（"光标移到边界就镜像反转"）。
// 收窄后光标须落在非透明像素（alpha>50，阴影/描边半透明不算）才进入悬停/翻转。
static bool isPetBodyPixelAt(const POINT& cur, const RECT& rc)
{
    if (cur.x < rc.left || cur.x >= rc.right || cur.y < rc.top || cur.y >= rc.bottom) return false;
    if (!g_dib_bits || g_winSize == 0) return false;
    const int px = cur.x - rc.left, py = cur.y - rc.top;
    if (px < 0 || py < 0 || px >= (int)g_winSize || py >= (int)g_winSize) return false;
    // g_dib_bits 是 BGRA（alpha 在 +3），值为上一帧渲染结果（1 帧滞后无感）
    return g_dib_bits[((size_t)py * g_winSize + px) * 4 + 3] > 50;
}
static void setPassthrough(bool enable)
{
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    bool cur = (ex & WS_EX_TRANSPARENT) != 0;
    if (cur == enable) return;
    ex = enable ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
    // §52 去掉 SWP_FRAMECHANGED：分层窗口（WS_EX_LAYERED）内容走 ULW、无边框，
    // FRAMECHANGED 会触发非客户区重算（首悬停切换时同步开销 → 沙漏候选），无收益
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---- 上传本地 riv：选文件 → 复制进皮肤库（重名加序号）→ import 验证 → 生效 ----
// 坏文件绝不落库：先复制到 __upload_tmp.riv，reloadSkinFromFile 校验通过才改名入库。
static bool uploadSkinDialog()
{
    log("[上传] 对话框已打开，等待选择文件...");
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    const bool en = petIsEnglish();
    ofn.lpstrFilter = en ? L"Rive model (*.riv)\0*.riv\0All files (*.*)\0*.*\0"
                         : L"Rive 模型 (*.riv)\0*.riv\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    BOOL ok = GetOpenFileNameW(&ofn);
    if (!ok)
    {
        DWORD err = CommDlgExtendedError();
        log("[上传] 对话框返回 FALSE（用户取消或错误=%lu）", err);
        return false;
    }
    std::wstring src(ofn.lpstrFile);
    log("[上传] 已选择: %s", wideToUtf8(src).c_str());
    size_t sep = src.find_last_of(L'\\');
    std::wstring fname = (sep == std::wstring::npos) ? src : src.substr(sep + 1);
    if (fname.empty()) { log("[上传] 文件名解析为空，中止"); return false; }
    const std::wstring dir = petSkinsDir();
    log("[上传] 皮肤库目录: %s", wideToUtf8(dir).c_str());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec); // 递归创建多层目录（CreateDirectoryW 不递归）
    if (ec) log("[上传] 创建目录警告: %d %s", ec.value(), ec.message().c_str());
    // 重名 → "name (n).riv"（不覆盖已有皮肤）
    std::wstring finalName = fname;
    std::wstring dest = dir + finalName;
    int n = 1;
    while (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        size_t dot = fname.find_last_of(L'.');
        std::wstring base = (dot == std::wstring::npos) ? fname : fname.substr(0, dot);
        std::wstring ext = (dot == std::wstring::npos) ? L"" : fname.substr(dot);
        finalName = base + L" (" + std::to_wstring(n) + L")" + ext;
        dest = dir + finalName;
        n++;
    }
    std::wstring tmp = dir + L"__upload_tmp.riv";
    log("[上传] 复制到临时文件: %s", wideToUtf8(tmp).c_str());
    if (!CopyFileW(src.c_str(), tmp.c_str(), FALSE))
    {
        DWORD err = GetLastError();
        log("[上传] 复制失败(err=%lu): %s", err, wideToUtf8(src).c_str());
        return false;
    }
    log("[上传] 复制成功，开始校验加载...");
    const std::string nameUtf8 = wideToUtf8(finalName);
    if (reloadSkinFromFile(tmp, nameUtf8, false))
    {
        MoveFileW(tmp.c_str(), dest.c_str());
        if (GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES)
            MoveFileW(tmp.c_str(), dest.c_str()); // 罕见失败重试一次
        log("[上传] 已入库并生效: %s（源: %s）", nameUtf8.c_str(), wideToUtf8(src).c_str());
        return true;
    }
    DeleteFileW(tmp.c_str()); // import 校验失败，清理临时文件，保持当前皮肤
    log("[上传] 文件校验未通过，未入库（保持当前皮肤）");
    return false;
}

// ---- 删除皮肤：确认后删文件；若删的是当前生效皮肤 → 自动切回出厂默认 ----
static void deleteSkin(const std::string& name)
{
    const bool en = petIsEnglish();
    const std::wstring p = petSkinPath(name);
    const std::wstring msg = en
        ? L"Delete skin \"" + utf8ToWide(name) + L"\"?"
        : L"确定删除皮肤「" + utf8ToWide(name) + L"」？";
    if (MessageBoxW(g_hwnd, msg.c_str(), en ? L"Delete Skin" : L"删除皮肤",
                    MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    if (!DeleteFileW(p.c_str())) { log("[删除] 删除失败: %s", name.c_str()); return; }
    log("[删除] 已删除皮肤: %s", name.c_str());
    if (g_skinName == name)
    {
        g_skinName.clear();
        reloadSkinFromFile(g_defaultSkinPath, "", false);
        log("[删除] 当前皮肤已被删除，已切回出厂默认");
    }
}

// ---- 菜单命令分发（自绘菜单选中后统一处理；原 TrackPopupMenu cmd switch 原样搬入）----
static void handleMenuCommand(int cmd)
{
    if (cmd >= 2000 && cmd < 2000 + (int)g_ab->stateMachineCount())
    {
        switchStateMachine((size_t)(cmd - 2000)); // 切换场景
    }
    else if (cmd == 3000)
    {
        if (!g_skinName.empty())
        {
            g_skinName.clear();
            savePetConfig();
            reloadSkinFromFile(g_defaultSkinPath, "", false);
        }
    }
    else if (cmd >= 3001 && cmd < 3001 + (int)listSkins().size())
    {
        const std::vector<std::string> skins = listSkins();
        const std::string& name = skins[cmd - 3001];
        if (g_skinName != name) reloadSkinFromFile(petSkinPath(name), name, false);
    }
    else if (cmd == 1004)
    {
        uploadSkinDialog();
    }
    else if (cmd >= 4000 && cmd < 4000 + (int)listSkins().size())
    {
        deleteSkin(listSkins()[cmd - 4000]);
    }
    else if (cmd == 1005)
    {
        // 恢复默认设置：皮肤→出厂默认、静音→关、按键显示→开、场景→默认策略(SM2)
        g_skinName.clear();
        g_muted = false;
        g_hudEnabled = true;
        if (g_engine) ma_engine_set_volume(g_engine->engine(), 1.0f);
        reloadSkinFromFile(g_defaultSkinPath, "", true);
        log("[菜单] 恢复默认设置");
    }
    else if (cmd == 1001)
    {
        g_muted = !g_muted;
        if (g_engine) // 加载线程音频初始化晚于 artboard 就绪，极小窗口内判空保护
            ma_engine_set_volume(g_engine->engine(), g_muted ? 0.0f : 1.0f);
        savePetConfig();
        log("[菜单] %s", g_muted ? "静音开（音量=0）" : "静音关（音量=1）");
    }
    else if (cmd == 1003)
    {
        g_hudEnabled = !g_hudEnabled;
        savePetConfig();
        log("[菜单] 按键显示%s", g_hudEnabled ? "开" : "关");
    }
    else if (cmd == 1002)
    {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
}

// ============================================================
// 自绘深色圆角菜单（对齐 Electron electron_full/menu.html：
// 黑底 rgba(32,32,32,.96) + 圆角 10 + hover 高亮 + 悬停右侧白蝶点缀）
// ============================================================
class PetMenuPopup;
static void InstallHooks();
static void UninstallHooks();
struct PetMenuItem {
    int id = 0;
    std::wstring label;
    bool checked = false;
    bool hasSub = false;              // 右侧 ▸
    bool isSep = false;
    std::vector<PetMenuItem> sub;     // 子菜单项
};

class PetMenuPopup {
public:
    enum { kItemH = 34, kSepH = 11, kPad = 6, kRadius = 10 };
    int width = 230;                  // 主菜单 230（Electron .menu），子菜单 210（.sub）
    HWND hwnd = nullptr;
    std::vector<PetMenuItem> items;
    int hover = -1;
    int openForIdx = -1;              // 子菜单对应的主菜单项索引（防闪烁幂等）
    bool destroyed = false;
    PetMenuPopup* child = nullptr;    // 当前子菜单（hover 弹出）

    double s = 1.0;   // §51 DPI 缩放（=g_dpiScale）：菜单自绘像素全部物理化，150% 屏不缩小
    PetMenuPopup() : items(), s(g_dpiScale) {}
    ~PetMenuPopup() { if (child) { delete child; child = nullptr; } ReleaseCache(); }

    // §52 菜单性能：Paint 每次全量重建 DIB + 2×GDI+Font 是"点击菜单卡"根因 →
    // 缓存 DC/DIB/Font，仅尺寸变化才重建 DIB、Font 只建一次，重绘只重画内容。
    HDC m_mdc = nullptr;
    HBITMAP m_bmp = nullptr;
    BYTE* m_bits = nullptr;
    int m_cachedW = 0, m_cachedH = 0;
    Gdiplus::Font* m_font = nullptr;
    Gdiplus::Font* m_fontB = nullptr;

    void ReleaseCache() {
        if (m_font) { delete m_font; m_font = nullptr; }
        if (m_fontB) { delete m_fontB; m_fontB = nullptr; }
        if (m_bmp) { DeleteObject(m_bmp); m_bmp = nullptr; }
        if (m_mdc) { DeleteDC(m_mdc); m_mdc = nullptr; }
        m_bits = nullptr; m_cachedW = m_cachedH = 0;
    }

    int Height() const {
        double h = 2 * kPad;
        for (auto& it : items) h += it.isSep ? kSepH : kItemH;
        return (int)(h * s);
    }

    static bool g_classDone;
    static void EnsureClass() {
        if (g_classDone) return;
        WNDCLASSW wc = {};
        wc.lpfnWndProc = &PetMenuPopup::MenuWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(IDC_ARROW));
        wc.lpszClassName = L"BeautypePetMenu";
        RegisterClassW(&wc);
        g_classDone = true;
    }

    // 打开主菜单：items 已填充；子菜单由 hover 动态创建
    void Show() {
        EnsureClass();
        RECT rc = { 0, 0, (int)(width * s), Height() };   // §51 物理尺寸（width 为逻辑 230）
        AdjustWindowRectEx(&rc, WS_POPUP, FALSE, WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        POINT pt;
        GetCursorPos(&pt);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        // 【根因】旧实现用 GetSystemMetrics(SM_CXSCREEN) 边界翻转 = 主屏宽度，
        // 双屏时鼠标在副屏、菜单被错误扔到主屏边缘。
        // 修复：MonitorFromPoint 取鼠标所在屏幕工作区，只在该屏幕内翻转。
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(mon, (LPMONITORINFO)&mi);
        RECT work = mi.rcWork;
        int x = pt.x, y = pt.y;
        if (x + w > work.right) x = work.right - w - 2;
        if (y + h > work.bottom) y = work.bottom - h - 2;
        if (x < work.left) x = work.left + 2;
        if (y < work.top) y = work.top + 2;
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                               L"BeautypePetMenu", L"", WS_POPUP,
                               x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)this);
        g_openMenu = this;
        InstallHooks();
        Paint();
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }

    void Close() {
        destroyed = true;
        if (child) { child->Close(); delete child; child = nullptr; }
        if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
        if (g_openMenu == this) {
            g_openMenu = nullptr;
            UninstallHooks();
        }
        ReleaseCache();   // §52 菜单关闭释放缓存 DIB/DC/Font
    }

    void CloseChild() {
        if (child) { child->Close(); delete child; child = nullptr; }
    }

    // ---- 绘制（ULW + GDI+，圆角外透明）----
    void Paint() {
        int h = Height();
        const float S = (float)s;                  // §51 DPI 缩放
        const int W = (int)(width * s);            // 物理宽度
        // §52 缓存 DIB/DC：尺寸未变只重画内容（旧实现每帧 CreateDIBSection + Font 是菜单卡根因）
        if (m_bmp && (m_cachedW != W || m_cachedH != h)) ReleaseCache();
        if (!m_bmp) {
            HDC winDc = GetDC(hwnd);
            m_mdc = CreateCompatibleDC(winDc);
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = W;
            bi.bmiHeader.biHeight = h;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            m_bmp = CreateDIBSection(m_mdc, &bi, DIB_RGB_COLORS, (void**)&m_bits, nullptr, 0);
            SelectObject(m_mdc, m_bmp);
            m_cachedW = W; m_cachedH = h;
            ReleaseDC(hwnd, winDc);
        }
        // 缓存 DIB 不复位 → 每帧先清透明背景，防旧像素残留
        memset(m_bits, 0, (size_t)W * h * 4);
        // GDI+ Font 构造昂贵（字体枚举+创建），只建一次（尺寸恒定 13.5*S）
        if (!m_font) m_font = new Gdiplus::Font(L"Segoe UI", 13.5f * S, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        if (!m_fontB) m_fontB = new Gdiplus::Font(L"Segoe UI", 13.5f * S, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

        Gdiplus::Graphics g(m_mdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        Gdiplus::GraphicsPath bgPath;
        float R = (float)(kRadius * s);
        Gdiplus::RectF br(0.5f, 0.5f, (float)(W - 1), (float)(h - 1));
        AddRoundRect(bgPath, br, R);
        // 阴影：外圈两层半透明描边（模拟 mockup 投影，无 GDI+ shadow 原生能力）
        Gdiplus::Pen sh1(Gdiplus::Color(60, 0, 0, 0), 5.0f * S);
        sh1.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&sh1, &bgPath);
        Gdiplus::Pen sh2(Gdiplus::Color(30, 0, 0, 0), 3.0f * S);
        sh2.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&sh2, &bgPath);
        // 黑底（mockup rgba(32,32,32,.96) ≈ alpha 245）
        Gdiplus::SolidBrush bg(Gdiplus::Color(245, 32, 32, 32));
        g.FillPath(&bg, &bgPath);
        // 边框 rgba(255,255,255,.15) ≈ 38
        Gdiplus::Pen border(Gdiplus::Color(38, 255, 255, 255), 1.0f * S);
        border.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawPath(&border, &bgPath);

        Gdiplus::SolidBrush fg(Gdiplus::Color(240, 240, 240, 240));
        Gdiplus::SolidBrush dim(Gdiplus::Color(154, 154, 154, 154));
        Gdiplus::SolidBrush checkBr(Gdiplus::Color(255, 127, 212, 255)); // #7fd4ff
        Gdiplus::SolidBrush hovBg(Gdiplus::Color(31, 255, 255, 255));    // hover rgba(255,255,255,.12)
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentNear);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        Gdiplus::Pen sepPen(Gdiplus::Color(31, 255, 255, 255), 1.0f);    // §52 分隔线 Pen 移出循环

        int y = (int)(kPad * s);
        for (size_t i = 0; i < items.size(); i++) {
            PetMenuItem& it = items[i];
            if (it.isSep) {
                float sy = (float)y + (float)(kSepH * s) * 0.5f;
                g.DrawLine(&sepPen,
                            (float)((kPad + 6.0) * s), sy, (float)((width - kPad - 6) * s), sy);
                y += (int)(kSepH * s);
                continue;
            }
            Gdiplus::RectF itemR((float)((kPad + 2) * s), (float)(y + 2 * s),
                                 (float)((width - 2 * kPad - 4) * s), (float)((kItemH - 4) * s));
            if ((int)i == hover) {
                Gdiplus::GraphicsPath hp;
                AddRoundRect(hp, itemR, 6.0f * S);
                g.FillPath(&hovBg, &hp);
            }
            // Electron 对齐：无左侧常驻图标；check(✓ #7fd4ff) 在文字左侧；hover 右侧白蝶点缀
            float tx = (float)((kPad + 12.0) * s);
            if (it.checked) {
                Gdiplus::RectF cr(tx, (float)y, 14.0f * S, (float)(kItemH * s));
                g.DrawString(L"\u2713", -1, m_fontB, cr, &fmt, &checkBr);
                tx += (float)(24.0 * s);
            }
            Gdiplus::RectF tr(tx, (float)y, (float)(W - tx - 30.0 * s), (float)(kItemH * s));
            g.DrawString(it.label.c_str(), -1, m_font, tr, &fmt, &fg);
            if (it.hasSub) {
                Gdiplus::RectF ar((float)((width - kPad - 12.0 - 8.0) * s), (float)y,
                                  16.0f * S, (float)(kItemH * s));
                g.DrawString(L"\u25B8", -1, m_font, ar, &fmt, &dim);     // ▸ 最右
            }
            if ((int)i == hover) {
                float hx = (float)((width - kPad - 10.0 - 6.5) * s);    // .mi:hover::after right:10px
                float hy = (float)(y + kItemH * s * 0.5);
                DrawHoverButterfly(g, hx, hy);
            }
            y += (int)(kItemH * s);
        }

        // ULW 提交（BGRA）
        SIZE sz = { W, h };
        POINT ptSrc = { 0, 0 };
        POINT ptDst = { 0, 0 };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        HDC scrDc = GetDC(nullptr);
        UpdateLayeredWindow(hwnd, scrDc, nullptr, &sz, m_mdc, &ptSrc, 0, &bf, ULW_ALPHA);
        ReleaseDC(nullptr, scrDc);
    }

    static void AddRoundRect(Gdiplus::GraphicsPath& p, const Gdiplus::RectF& r, float rad) {
        float d = rad * 2;
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        p.CloseFigure();
    }

    // 悬停右侧蝴蝶点缀（复刻 Electron .mi:hover::after：
    // beautype-logo 四圆上大下小，全白，13px，等比例 viewBox 48）
    static void DrawHoverButterfly(Gdiplus::Graphics& g, float cx, float cy) {
        const float s = 13.0f / 48.0f;
        Gdiplus::SolidBrush w(Gdiplus::Color(255, 255, 255, 255));
        g.FillEllipse(&w, cx + (15 - 24) * s - 11 * s, cy + (14 - 24) * s - 11 * s, 22 * s, 22 * s);
        g.FillEllipse(&w, cx + (33 - 24) * s - 11 * s, cy + (14 - 24) * s - 11 * s, 22 * s, 22 * s);
        g.FillEllipse(&w, cx + (18 - 24) * s - 8 * s,  cy + (36 - 24) * s - 8 * s,  16 * s, 16 * s);
        g.FillEllipse(&w, cx + (30 - 24) * s - 8 * s,  cy + (36 - 24) * s - 8 * s,  16 * s, 16 * s);
    }

    // hover 到带子菜单项时：右侧弹出子菜单窗口（同项不重建，防闪烁）
    void OpenChild(const std::vector<PetMenuItem>& sub, int forIdx) {
        if (child && !child->destroyed && child->openForIdx == forIdx) return; // 幂等
        CloseChild();
        if (sub.empty()) return;
        PetMenuPopup* c = new PetMenuPopup();
        c->width = 210;
        c->openForIdx = forIdx;
        c->items = sub;
        RECT rc = { 0, 0, (int)(c->width * c->s), c->Height() };   // §51 物理尺寸
        AdjustWindowRectEx(&rc, WS_POPUP, FALSE, WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
        POINT pt;
        GetCursorPos(&pt);
        RECT mr;
        GetWindowRect(hwnd, &mr);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        // 子菜单 y 对齐 hover 项（成熟做法：跟随左侧选项位置，而非贴主菜单顶部）
        int itemTop = (int)(kPad * s);                             // §51 缩放后偏移
        for (int i = 0; i < forIdx && i < (int)items.size(); i++)
            itemTop += (int)((items[i].isSep ? kSepH : kItemH) * s);
        int x = mr.right - 2, y = mr.top + itemTop;
        if (x + w > GetSystemMetrics(SM_CXSCREEN)) x = mr.left - w + 2;
        if (y + h > GetSystemMetrics(SM_CYSCREEN)) y = GetSystemMetrics(SM_CYSCREEN) - h - 2;
        c->hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                                  L"BeautypePetMenu", L"", WS_POPUP,
                                  x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), c);
        SetWindowLongPtrW(c->hwnd, GWLP_USERDATA, (LONG_PTR)c);
        c->Paint();
        ShowWindow(c->hwnd, SW_SHOWNOACTIVATE);
        child = c;
    }

    static LRESULT CALLBACK MenuWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        PetMenuPopup* self = (PetMenuPopup*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            POINT pt;
            GetCursorPos(&pt);
            RECT rc;
            GetWindowRect(h, &rc);
            int idx = -1;
            if (pt.x >= rc.left && pt.x <= rc.right && pt.y >= rc.top && pt.y <= rc.bottom) {
                int y = (pt.y - rc.top) - (int)(kPad * self->s);   // §51 命中行高物理化
                int acc = 0;
                for (size_t i = 0; i < self->items.size(); i++) {
                    int hh = (int)((self->items[i].isSep ? kSepH : kItemH) * self->s);
                    if (y >= acc && y < acc + hh) { idx = (int)i; break; }
                    acc += hh;
                }
            }
            if (idx != self->hover) {
                self->hover = idx;
                self->Paint();
            }
            // 子菜单：hover 带子项 → 弹出；否则关闭子菜单
            bool overChild = false;
            if (self->child && self->child->hwnd) {
                RECT cr;
                GetWindowRect(self->child->hwnd, &cr);
                if (pt.x >= cr.left && pt.x <= cr.right && pt.y >= cr.top && pt.y <= cr.bottom)
                    overChild = true;
            }
            bool overSubItem = (idx >= 0 && !self->items[idx].isSep && self->items[idx].hasSub);
            if (overSubItem && !overChild) self->OpenChild(self->items[idx].sub, idx);
            else if (!overSubItem && !overChild) self->CloseChild();
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt;
            GetCursorPos(&pt);
            RECT rc;
            GetWindowRect(h, &rc);
            int idx = -1;
            int y = (pt.y - rc.top) - (int)(kPad * self->s);   // §51 命中行高物理化
            int acc = 0;
            for (size_t i = 0; i < self->items.size(); i++) {
                int hh = (int)((self->items[i].isSep ? kSepH : kItemH) * self->s);
                if (y >= acc && y < acc + hh) { idx = (int)i; break; }
                acc += hh;
            }
            if (idx >= 0 && !self->items[idx].isSep) {
                PetMenuItem& it = self->items[idx];
                if (it.hasSub) {
                    self->OpenChild(it.sub, idx);
                } else if (it.id > 0) {
                    int cmd = it.id;
                    PetMenuPopup* rootM = g_openMenu;   // 子菜单命中也要关整棵菜单
                    if (rootM) rootM->Close();
                    handleMenuCommand(cmd);
                }
            }
            return 0;
        }
        case WM_DESTROY:
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};
bool PetMenuPopup::g_classDone = false;

// ---- 菜单打开期间的全局鼠标/键盘钩子：点击菜单外或按 Esc 关闭 ----
#define WM_APP_CLOSE_MENU (WM_APP + 1)
static HHOOK g_menuMouseHook = nullptr;
static HHOOK g_menuKbdHook = nullptr;

static LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN && g_openMenu)
    {
        MSLLHOOKSTRUCT* ms = (MSLLHOOKSTRUCT*)lParam;
        bool inside = false;
        for (PetMenuPopup* m = g_openMenu; m; m = m->child)
        {
            if (m->hwnd)
            {
                RECT r;
                GetWindowRect(m->hwnd, &r);
                if (ms->pt.x >= r.left && ms->pt.x <= r.right && ms->pt.y >= r.top && ms->pt.y <= r.bottom)
                { inside = true; break; }
            }
        }
        if (!inside) PostMessageW(g_hwnd, WM_APP_CLOSE_MENU, 0, 0);
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
static LRESULT CALLBACK LowLevelKbdProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION && g_openMenu)
    {
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        if (kb->vkCode == VK_ESCAPE) PostMessageW(g_hwnd, WM_APP_CLOSE_MENU, 0, 0);
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
static void InstallHooks()
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!g_menuMouseHook) g_menuMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hInst, 0);
    if (!g_menuKbdHook) g_menuKbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKbdProc, hInst, 0);
}
static void UninstallHooks()
{
    if (g_menuMouseHook) { UnhookWindowsHookEx(g_menuMouseHook); g_menuMouseHook = nullptr; }
    if (g_menuKbdHook) { UnhookWindowsHookEx(g_menuKbdHook); g_menuKbdHook = nullptr; }
}

// 构建右键菜单项并弹出自绘菜单（替代原 TrackPopupMenu 原生菜单）
static void showPetMenu()
{
    // 重复打开前先关闭旧菜单（防菜单窗口累积/泄漏；外部点击/Esc 由 LL 钩子关）
    if (g_openMenu) g_openMenu->Close();
    // §57 打开菜单前重读语言文件（其他进程刚切语言，本进程下次开菜单即时生效）
    const bool en = petIsEnglish();
    // 加载中（riv 未就绪，g_ab/g_smi 为 null）：只允许退出，避免空指针
    if (!g_ab || !g_smi) {
        PetMenuPopup* m = new PetMenuPopup();
        PetMenuItem it;
        it.id = 1002; it.label = en ? L"Exit Pet" : L"退出桌宠";
        m->items.push_back(it);
        m->Show();
        return;
    }
    std::vector<PetMenuItem> items;
    // 切换场景（子菜单：全部状态机，当前打勾）
    PetMenuItem mi;
    mi.label = en ? L"Switch Scene" : L"切换场景";
    mi.hasSub = true;
    for (size_t i = 0; i < g_ab->stateMachineCount(); i++) {
        PetMenuItem s;
        s.id = (int)(2000 + i);
        s.label = utf8ToWide(g_ab->stateMachine(i)->name());
        s.checked = (i == g_curSmIndex);
        mi.sub.push_back(s);
    }
    items.push_back(mi);
    PetMenuItem sep; sep.isSep = true; items.push_back(sep);
    // 切换皮肤（子菜单：默认 + 皮肤库，当前打勾）
    PetMenuItem ms;
    ms.label = en ? L"Switch Skin" : L"切换皮肤";
    ms.hasSub = true;
    {
        PetMenuItem s; s.id = 3000;
        s.label = en ? L"Default Skin" : L"默认皮肤"; s.checked = g_skinName.empty();
        ms.sub.push_back(s);
        const std::vector<std::string> skins = listSkins();
        for (size_t i = 0; i < skins.size(); i++) {
            PetMenuItem s2;
            s2.id = (int)(3001 + i);
            s2.label = utf8ToWide(skins[i]);
            s2.checked = (g_skinName == skins[i]);
            ms.sub.push_back(s2);
        }
    }
    items.push_back(ms);
    // 上传本地 riv
    PetMenuItem mu;
    mu.id = 1004;
    mu.label = en ? L"Upload Local .riv\u2026" : L"上传本地 riv\u2026";
    items.push_back(mu);
    // 删除皮肤（子菜单，始终显示；hover 空列表不弹出，对齐 Electron）
    const std::vector<std::string> skins = listSkins();
    {
        PetMenuItem md;
        md.label = en ? L"Delete Skin" : L"删除皮肤";
        md.hasSub = true;
        for (size_t i = 0; i < skins.size(); i++) {
            PetMenuItem s;
            s.id = (int)(4000 + i);
            s.label = utf8ToWide(skins[i]);
            md.sub.push_back(s);
        }
        items.push_back(md);
    }
    // 恢复默认设置
    PetMenuItem mrs;
    mrs.id = 1005;
    mrs.label = en ? L"Restore Defaults" : L"恢复默认设置";
    items.push_back(mrs);
    PetMenuItem sep2; sep2.isSep = true; items.push_back(sep2);
    // 按键显示 / 静音（check）
    PetMenuItem mh;
    mh.id = 1003;
    mh.label = en ? L"Show Keystrokes" : L"按键显示";
    mh.checked = g_hudEnabled;
    items.push_back(mh);
    PetMenuItem mm;
    mm.id = 1001;
    mm.label = en ? L"Mute" : L"静音";
    mm.checked = g_muted;
    items.push_back(mm);
    PetMenuItem sep3; sep3.isSep = true; items.push_back(sep3);
    // 退出桌宠
    PetMenuItem mx;
    mx.id = 1002;
    mx.label = en ? L"Exit Pet" : L"退出桌宠";
    items.push_back(mx);

    PetMenuPopup* m = new PetMenuPopup();
    m->items = items;
    m->Show();
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_APP_CLOSE_MENU:
        if (g_openMenu) g_openMenu->Close();   // 菜单外点击 / Esc
        return 0;
    case WM_TIMER:
        if (wp == 1)
        {
            // 【drag 手势修复】旧设计（16.29）拖拽中冻结渲染防闪烁：
            // SetWindowPos 移动 ULW 窗口与 UpdateLayeredWindow 贴位图跨消息帧交替会互相
            // 竞争（屏幕空帧/闪）。但冻结 = renderFrame 不跑 = g_smi->advanceAndApply 不
            // 跑 = drag=true 永不被状态机处理 → 抱住动画不触发（用户实测）。
            // 新方案：拖拽中在同一个 WM_TIMER tick 内先移窗再 renderFrame，
            // SetWindowPos 与 UpdateLayeredWindow 同消息、顺序执行不跨帧竞争 → 不闪，
            // 且状态机每帧推进、drag=true → 抱住 正常切换。
            if (g_dragging)
            {
                POINT cur; GetCursorPos(&cur);
                SetWindowPos(hwnd, nullptr, cur.x - g_dragOff.x, cur.y - g_dragOff.y,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                renderFrame();
                return 0;
            }
            // 【闪烁 16.24】滚轮缩放合并：快速滚动只累计目标大小，停顿 120ms 后
            // 才重建一次（连续重建是缩放连续闪的来源）
            bool didResize = false;
            // 【闪烁 16.29 同机制】缩放第二拍：第一拍（换资源+移位置）已完成，
            // 本拍渲染——移动已停，SetWindowPos 与 ULW 严格不同帧，无竞争不闪。
            // 若滚动仍在继续且目标又变 → 重做第一拍换目标（跳过过期中间尺寸）。
            if (g_resizeRenderPending)
            {
                if (g_resizePending && g_resizeTarget != g_resizeAppliedTarget)
                {
                    resizePetPhase1(g_resizeTarget);
                }
                else
                {
                    g_resizeRenderPending = false;
                    g_resizePending = false;       // 目标已应用，清除合并标记
                    renderFrame();                 // ULW 一次性改大小+内容（位置已就位）
                    savePetConfig();               // 合并落盘（原每格一存）
                }
                didResize = true;
            }
            if (!didResize && g_resizePending && GetTickCount() - g_lastResizeTick >= 120)
            {
                g_resizePending = false;
                g_lastResizeTick = GetTickCount();
                resizePetPhase1(g_resizeTarget);   // 第一拍：移位置+换资源，不渲染
                g_resizeRenderPending = true;      // 下一拍渲染
                didResize = true;
            }
            if (!didResize) renderFrame();
            // 调试: 自动切换状态机（复现切换后渲染问题）
            if (g_autoSwitchSec > 0 && !g_autoSwitchDone &&
                GetTickCount() - g_autoSwitchStart >= (DWORD)g_autoSwitchSec * 1000)
            {
                g_autoSwitchDone = true;
                log("[调试] 自动切换 → 状态机[%u]", g_autoSwitchTarget);
                switchStateMachine(g_autoSwitchTarget);
            }
            // 悬停检测: 光标在身体像素上 -> 可交互 + pointerMove 喂状态机; 离开 -> 穿透 + pointerExit
            // §52 判定收窄到非透明身体像素（isPetBodyPixelAt），透明边角不再触发翻转/交互
            if (!g_dragging)
            {
                POINT cur;
                GetCursorPos(&cur);
                RECT rc;
                GetWindowRect(hwnd, &rc);
                bool inside = isPetBodyPixelAt(cur, rc);
                if (inside != g_hover)
                {
                    g_hover = inside;
                    setPassthrough(!inside);
                    if (inside) petPointerMove(cur.x, cur.y);
                    else petPointerExit();
                    // 悬停输入（非契约）不绑定不写值：由皮肤内 Hit Area + hover
                    // 监听器自行驱动（pointerMove/Exit 已喂给状态机，预览即生效）
                    log("[交互] 鼠标%s窗口（悬停动画由皮肤 Hit Area 内部驱动）",
                        inside ? "进入" : "离开");
                }
                else if (inside)
                {
                    petPointerMove(cur.x, cur.y); // 持续喂移动（眼睛跟随/悬停鼻子）
                }
            }
        }
        return 0;
    case WM_CONTEXTMENU:
        setPassthrough(false);
        showPetMenu();
        return 0;
    case WM_LBUTTONDOWN:
        if (g_openMenu) { g_openMenu->Close(); return 0; }  // 菜单打开时点击外部 → 关闭
        setPassthrough(false);
        g_dragging = true;
        GetCursorPos(&g_downPos);
        petPointerDown(g_downPos.x, g_downPos.y); // 喂指针按下（点击奶茶区域等）
        if (g_dragInput) g_dragInput->value(true); // 契约 drag：按住身体=true
        SetCapture(hwnd);
        {
            POINT cur; GetCursorPos(&cur);
            RECT rc; GetWindowRect(hwnd, &rc);
            g_dragOff.x = cur.x - rc.left;
            g_dragOff.y = cur.y - rc.top;
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging)
        {
            g_dragging = false;
            ReleaseCapture();
            POINT cur; GetCursorPos(&cur);
            // 【闪烁 16.29】松手不立即 renderFrame：SetWindowPos（移动）刚停，若同帧
            // renderFrame（ULW 贴位图）仍会竞争 → 松手闪一下；交给 WM_TIMER 下一拍
            // 渲染（移动已完全停止，无竞争）——画面保持冻结帧 ≤33ms 后自然恢复。
            savePetConfig(); // 拖动结束 → 持久化新位置（热启动恢复）
            petPointerUp(cur.x, cur.y); // 喂指针抬起（Rive 内部配对 Down/Up）
            if (g_dragInput) g_dragInput->value(false); // 契约 drag：松开=false
            // 点击交互（非契约，生气/点击奶茶等）不再由 runtime 写 bool：皮肤内
            // Hit Area + click 监听器自行响应（Down/Up 已配对喂给状态机）；全局
            // 左键的契约 click 输入由 WH_MOUSE_LL 钩子独立驱动（800ms 保持）。
        }
        return 0;
    case WM_MBUTTONUP:
        // 【置顶 4.8】鼠标中键 → 重回置顶：窗口创建即 WS_EX_TOPMOST，但可能被其他
        // 置顶窗口（播放器/悬浮工具等）压住；SetWindowPos(HWND_TOPMOST) 把它重新
        // 提到 TOPMOST 组顶部（找回置顶，不移动/不缩放/不激活）
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
        log("[置顶] 鼠标中键 → 重回置顶（TOPMOST 组顶部）");
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging)
        {
            // 拖拽中窗口移动 + renderFrame 统一在 WM_TIMER tick 原子执行（见 WM_TIMER
            // drag 分支）。这里不再 SetWindowPos，避免与 ULW 贴位图跨帧竞争。
        }
        return 0;
    case WM_MOUSEWHEEL:
    {
        short delta = GET_WHEEL_DELTA_WPARAM(wp);
        // 【闪烁 16.24】滚轮只记目标大小，由 WM_TIMER 停顿后一次重建（合并连续滚轮）
        UINT target = (UINT)((int)g_winSize + (delta > 0 ? PET_SIZE_STEP : -PET_SIZE_STEP));
        target = std::max<UINT>((UINT)PET_MIN_SIZE, std::min<UINT>((UINT)PET_MAX_SIZE, target));
        if (target != g_winSize)
        {
            g_resizeTarget = target;
            g_resizePending = true;
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { PostMessageW(hwnd, WM_CLOSE, 0, 0); }
        return 0;
    case WM_DESTROY:
        savePetConfig(); // 退出兜底保存（菜单切换已实时保存）
        DeleteFileW(petLockPath(g_petSlot).c_str()); // 多开：释放槽位锁（配置保留）
        g_exit = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ==================== M4-1 独立输入兜底：全局低级键盘钩子 ====================
// 目标电脑未装 Beautype 输入法时 IPC 链路永久待机（端口文件不存在）；钩子直接
// 监听系统键盘，把 vk 事件喂给与 IPC type=3 相同的 hudKeyDown/hudKeyUp +
// istyping 脉冲。与 IPC 链路并存互补：同一物理键两来源都触发时 HUD map 同
// label 刷新 down_ms 无害；锁/脉冲均为原子。边界：UAC 提权窗口/锁屏/安全桌面
// 收不到（Windows 安全设计，此时桌宠也被系统隔离，影响可忽略）。
static HHOOK g_kbdHook = nullptr;

static LRESULT CALLBACK kbdHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        const KBDLLHOOKSTRUCT* kb = (const KBDLLHOOKSTRUCT*)lParam;
        // 2026-10-17：跳过内部模拟键 VK_SELECT(0x29)——点击候选时服务端
        // SendInput 模拟该键（LL 钩子同样捕获），不喂 HUD / 不触发打字脉冲
        // （用户实测点击候选桌宠冒 "key41"；真实键盘不存在 VK_SELECT 键）。
        if (kb && kb->vkCode > 0 && kb->vkCode <= 255 && kb->vkCode != VK_SELECT)
        {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
            {
                InterlockedExchange(&g_typingPulse, 1);
                hudKeyDown((int)kb->vkCode);
            }
            else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
            {
                hudKeyUp((int)kb->vkCode);
            }
        }
    }
    return CallNextHookEx(g_kbdHook, code, wParam, lParam);
}

// ---- 钩子自愈（2026-10-20 §49）----
// WH_KEYBOARD_LL 钩子回调若被系统判定超时（默认 LowLevelHooksTimeout=1000ms）
// 会被静默卸载且不会自动恢复 → keyup 永久丢失 → HUD 按键卡住。HUD 卡住清除
// （keyup 丢失兜底）累计 kHudKeyupLossRebuildAt 次即重装。重装在主线程（绘制
// 循环内）执行，与钩子回调同线程派发，无并发风险。
static void rebuildKbdHook()
{
    if (g_kbdHook)
    {
        UnhookWindowsHookEx(g_kbdHook);
        g_kbdHook = nullptr;
    }
    g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, kbdHookProc, GetModuleHandleW(nullptr), 0);
    if (g_kbdHook)
        log("[HUD] LL键盘钩子已重装（自愈，keyup丢失触发）");
    else
        log("[HUD] LL键盘钩子重装失败 GLE=%lu", (unsigned long)GetLastError());
}

// ==================== M4-1b 全局鼠标钩子：click 输入（点击脉冲） ====================
// 与 kbdHookProc 同款：WH_MOUSE_LL 事件驱动，任意左键按下（全局，窗口外也有效）
// 置 click 脉冲 → updateInputs 消抖成 click=true + 短保持。选钩子而非 30fps
// GetAsyncKeyState 轮询：脉冲语义下轮询会漏掉 <33ms 的快速点击（事件驱动不漏）。
static HHOOK g_mouseHook = nullptr;

static LRESULT CALLBACK mouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == WM_LBUTTONDOWN)
    {
        InterlockedExchange(&g_clickPulse, 1);
    }
    return CallNextHookEx(g_mouseHook, code, wParam, lParam);
}

// ==================== M3-4 输入法事件链路：桌宠侧 TCP 客户端（协议 v1） ====================
// 端口发现: 读 %LOCALAPPDATA%\rime_beautify\runtime\ime.port.json
//   {"protocol_version":1,"port":<port>,"token":"<hex>"}
// 鉴权: 连接后先发 hello {v:1,type:1,token:"..."}（IME 校验失败立即断开）
// 消息: type=3 key_event {vk,scan,make,ts} → make=1(按下) → 置 istyping 脉冲
// 重连: 断线后重读端口文件，退避 1s→2s→4s→…上限 30s
// 帧: 4 字节大端长度 + UTF-8 JSON 载荷（docs\ipc-protocol.md）
namespace {

std::string ipcReadFile(const std::wstring& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

std::wstring ipcPortFilePath()
{
    std::wstring base = knownLocalAppData();
    if (base.empty()) return L"";
    return base + L"\\rime_beautify\\runtime\\ime.port.json";
}

std::vector<uint8_t> ipcFrame(const std::string& json)
{
    uint32_t len = (uint32_t)json.size();
    std::vector<uint8_t> f(4 + len);
    f[0] = (uint8_t)(len >> 24); f[1] = (uint8_t)(len >> 16);
    f[2] = (uint8_t)(len >> 8);  f[3] = (uint8_t)(len & 0xFF);
    memcpy(f.data() + 4, json.data(), len);
    return f;
}

bool recvAll(SOCKET s, uint8_t* buf, size_t n)
{
    size_t got = 0;
    while (got < n)
    {
        int r = recv(s, (char*)buf + got, (int)(n - got), 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

// 用户主动退出（右键"退出桌宠"）：主线程独立连接发 bye。
// 为什么不用 IPC 线程发：IPC 线程可能正阻塞在 recv（等 IME 数据），g_exit 无法
// 中断阻塞的 recv → bye 永远发不出（真机验证：server 只收到"连接断开"没有
// "用户已主动退出桌宠"）。主线程新建连接 → hello 鉴权 → bye，不依赖线程状态。
// 失败静默（IME 没在运行/端口文件缺失时无副作用）。
static void sendByeFromMain()
{
    const std::wstring path = ipcPortFilePath();
    const std::string s = ipcReadFile(path);
    if (s.empty()) return;
    long long port = jsonNum(s, "port");
    const std::string token = jsonStr(s, "token");
    if (port <= 0 || port > 65535 || token.empty()) return;
    SOCKET sk = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sk == INVALID_SOCKET) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((u_short)port);
    if (connect(sk, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR)
    {
        closesocket(sk);
        return;
    }
    std::string hello = "{\"v\":1,\"type\":1,\"token\":\"" + token + "\"}";
    auto hf = ipcFrame(hello);
    auto bf = ipcFrame("{\"v\":1,\"type\":6}");
    send(sk, (const char*)hf.data(), (int)hf.size(), 0);
    // 关键：等 server 鉴权完成再发 bye（实测假桌宠 hello→等 200ms→bye 必触发；
    // 连续发送+立即 close 会因 RST/缓冲竞争丢 bye——真机验证过的坑）
    Sleep(200);
    send(sk, (const char*)bf.data(), (int)bf.size(), 0);
    Sleep(200); // 让 bye 帧真正送达后再关（closesocket 太快可能吞帧）
    closesocket(sk);
    log("[IPC] 主线程已发送 bye（用户主动退出）");
}

DWORD WINAPI petIpcThread(LPVOID)
{
    int backoffMs = 1000;
    while (!g_exit)
    {
        // 1) 读端口文件
        std::string cfg = ipcReadFile(ipcPortFilePath());
        long long port = cfg.empty() ? -1 : jsonNum(cfg, "port");
        std::string token = cfg.empty() ? std::string() : jsonStr(cfg, "token");
        if (port <= 0 || port > 65535)
        {
            Sleep(backoffMs);
            backoffMs = std::min(backoffMs * 2, 30000);
            continue; // 输入法未启动，待机
        }
        // 2) 连接
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { Sleep(backoffMs); backoffMs = std::min(backoffMs * 2, 30000); continue; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((u_short)port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
        {
            closesocket(s);
            Sleep(backoffMs);
            backoffMs = std::min(backoffMs * 2, 30000);
            continue;
        }
        backoffMs = 1000;
        // 3) 鉴权 hello
        std::string hello = "{\"v\":1,\"type\":1,\"token\":\"" + token + "\"}";
        auto hf = ipcFrame(hello);
        if (send(s, (const char*)hf.data(), (int)hf.size(), 0) == SOCKET_ERROR)
        {
            closesocket(s);
            Sleep(1000);
            continue;
        }
        log("[IPC] 已连接输入法端口 %lld，等待按键事件", port);
        // 4) 收帧循环
        while (!g_exit)
        {
            uint8_t hdr[4];
            if (!recvAll(s, hdr, 4)) break;
            uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                           ((uint32_t)hdr[2] << 8) | (uint32_t)hdr[3];
            if (len > (1u << 20)) break; // 超上限视为协议错误
            std::vector<uint8_t> buf(len);
            if (!recvAll(s, buf.data(), len)) break;
            std::string json((const char*)buf.data(), len);
            long long type = jsonNum(json, "type");
            if (type == 3)
            {
                long long make = jsonNum(json, "make");
                if (make == 1) // 键按下（keyup make=0 不触发——消抖节奏）
                {
                    InterlockedExchange(&g_typingPulse, 1);
                    // 注：HUD 按键卡片由 LL 键盘钩子(kbdHookProc)单一来源提供。
                    // IPC 的 vk 是 TSF 翻译后的"字符 vk"（NumPad+ → 187"= "，NumPad- → 189"-"），
                    // 与钩子拿到的原始 vkCode（107/109）不一致——双通道会各出一张卡片
                    // （实测小键盘 +/- 额外显示"="），故此处不再喂 HUD。
                }
            }
            else if (type == 6) // bye
                break;
            // type=19 显隐已废弃（用户定稿：切走不隐藏，桌宠常驻，退出靠右键）
        }
        // 用户主动退出（右键"退出桌宠"→ WM_DESTROY → g_exit）：发 bye 通知
        // server"用户已主动退出"，本次服务生命周期不再自动拉起（重启服务恢复）。
        if (g_exit)
        {
            std::string bye = "{\"v\":1,\"type\":6}";
            auto bf = ipcFrame(bye);
            send(s, (const char*)bf.data(), (int)bf.size(), 0);
        }
        closesocket(s);
        log("[IPC] 连接断开，退避重连 %dms", backoffMs);
        Sleep(backoffMs);
        backoffMs = std::min(backoffMs * 2, 30000);
    }
    return 0;
}

}  // namespace

// ---- 启动加载线程：riv 解析 + artboard + 状态机 + 音频（纯 CPU，见全局注释） ----
// 主线程先创建窗口并持续绘制 DotsJump 加载动画，本线程就绪后下一帧自动切正常渲染。
// 失败：PostMessage(WM_CLOSE) 让窗口退出（与原"return 1 退出"行为一致）。
struct PetLoadArgs {
    std::vector<uint8_t> bytes;
    int smIndex = 0;           // 初始状态机索引（CLI/配置）
    UINT autoSwitchSec = 0;    // 自动切换调试参数
    UINT autoSwitchTarget = 0;
};
static DWORD WINAPI petLoadThread(LPVOID p)
{
    std::unique_ptr<PetLoadArgs> a(static_cast<PetLoadArgs*>(p));
    rive::Span<const uint8_t> span(a->bytes.data(), a->bytes.size());
    // 裸指针持有 File（生命周期=进程；switchStateMachine 切换场景时 artboardDefault 重建实例）
    g_file = rive::File::import(span, g_rc).release();
    if (!g_file) { log("[FAIL] File::import"); PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return 1; }
    log("[步进] import OK");
    auto ab = g_file->artboardDefault();
    if (!ab) { log("[FAIL] artboardDefault"); PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return 1; }
    g_ab = ab.release();
    log("[OK] artboard \"%s\" %.0fx%.0f 状态机=%zu 音频=%s",
        g_ab->name().c_str(), g_ab->width(), g_ab->height(),
        g_ab->stateMachineCount(), g_ab->hasAudio() ? "有" : "无");

    if (g_ab->stateMachineCount() > 0)
    {
        // 默认场景策略：CLI 显式指定（smIndex>0 且合法）优先 > 状态机≥2 取 SM2
        // （索引1，避开 boo2 的 SM1 空白坑）> 索引0；config 场景名在 loadPetConfig
        // 里最高优先（热启动回到上次场景）
        size_t initialSm = g_ab->stateMachineCount() >= 2 ? 1 : 0;
        if (a->smIndex > 0 && (size_t)a->smIndex < g_ab->stateMachineCount())
            initialSm = (size_t)a->smIndex;
        if (a->autoSwitchSec > 0)
        {
            g_autoSwitchSec = a->autoSwitchSec;
            g_autoSwitchTarget = a->autoSwitchTarget;
            g_autoSwitchStart = GetTickCount();
            log("[调试] %u 秒后自动切换 → 状态机[%u]", g_autoSwitchSec, g_autoSwitchTarget);
        }
        // 状态恢复：配置里的场景名优先于 CLI 参数（热启动回到上次场景）
        loadPetConfig(&initialSm, g_ab->stateMachineCount());
        switchStateMachine(initialSm); // 默认场景 = 状态机[0]（含输入/监听器绑定）
        log("[步进] switchStateMachine OK");
    }

    // ---- 音频：启动 + 预热（修复"首次点击无声音"：miniaudio 设备异步启动，首声会丢） ----
    g_engine = rive::AudioEngine::RuntimeEngine(true);
    log("[步进] AudioEngine 创建=%s", g_engine ? "OK" : "null");
    if (g_engine)
    {
        g_engine->start();
        Sleep(80); // 等设备数据回调建立
        {
            const uint32_t sr = rive::AudioEngine::defaultSampleRate;
            const uint32_t ch = rive::AudioEngine::defaultNumChannels;
            const uint32_t frames = sr / 10; // 100ms 静音
            std::vector<float> silence((size_t)frames * ch, 0.0f);
            auto src = rive::rcp<rive::AudioSource>(new rive::AudioSource(
                rive::Span<float>(silence.data(), silence.size()), ch, sr));
            g_engine->play(src, 0, frames, 0);
        }
        log("[OK] 音频引擎启动 + 预热 100ms（首声不丢）");
        if (g_muted) // 恢复静音状态（配置加载在本线程完成）
            ma_engine_set_volume(g_engine->engine(), 0.0f);
    }

    g_loadOk = true;
    g_loadDoneTick = GetTickCount();
    log("[OK] 启动加载完成（%.2f 秒），切换到桌宠渲染",
        (double)(g_loadDoneTick - g_loadingStart) / 1000.0);
    return 0;
}

// ---- musicon 契约输入：WASAPI 会话级"正在播放音乐"检测（2026-10-19）----
// 实现依据（官方实证，来源链接见 docs/errors-and-lessons.md §43 复盘）：
// * 微软音频工程师 meters.exe 模式（WASAPI 官方示例作者）：IMMDeviceEnumerator →
//   GetDefaultAudioEndpoint(eRender) → Activate(IAudioSessionManager2) →
//   GetSessionEnumerator → 逐会话 GetState()==Active（快路径）→
//   QI IAudioSessionControl2：IsSystemSoundsSession() 排除系统通知会话 →
//   QI IAudioMeterInformation：GetPeakValue()（会话级峰值，端点音量衰减前采样，
//   与 App/系统音量滑条无关）——即"会话级峰值检测"。
// * "音量门槛"方案已证伪（App 音量与系统音量独立，门槛代表不了"正在播放"）；
//   本实现只判"信号能量存在"（peak > kMusiconPeakEpsilon，≈-66dBFS），非音量高低。
// * 500ms 去抖：每 kMusiconPollMs=250ms 采样，连续 kMusiconDebounceSamples=2 次
//   一致才翻转（短促音/系统残响不误触发）。
// * 排除本进程自身会话：桌宠自己的音效不算"外部正在播放"。
static DWORD WINAPI musiconThreadProc(LPVOID)
{
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // 心跳1：线程进入（定位"CreateThread 成功但线程从未执行"类问题）
    log("[输入] musicon 线程已进入，CoInitializeEx=0x%08X，PID=%lu", initHr, GetCurrentProcessId());
    bool last = false;      // 上一采样原始值
    int stable = 0;         // 候选状态连续一致次数（>=2 才可翻转）
    DWORD lastWarnTick = 0; // 告警限频（轮询 250ms，告警最多每 5s 一条）
    int polls = 0;          // 心跳计数（前 3 轮打印枚举摘要）
    while (!g_exit)
    {
        bool audible = false;
        int sessCount = -1;      // 心跳：枚举到的会话数（-1=枚举失败）
        HRESULT hr = S_OK;
        IMMDeviceEnumerator* pEnum = nullptr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
        if (SUCCEEDED(hr) && pEnum)
        {
            IMMDevice* pDev = nullptr;
            hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
            if (SUCCEEDED(hr) && pDev)
            {
                IAudioSessionManager2* pMgr = nullptr;
                hr = pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                    nullptr, (void**)&pMgr);
                if (SUCCEEDED(hr) && pMgr)
                {
                    IAudioSessionEnumerator* pSess = nullptr;
                    hr = pMgr->GetSessionEnumerator(&pSess);
                    if (SUCCEEDED(hr) && pSess)
                    {
                        int cnt = 0;
                        if (SUCCEEDED(pSess->GetCount(&cnt)))
                        {
                            sessCount = cnt;
                            const bool dbg = (polls <= 5); // 前 5 轮打印逐会话明细
                            for (int i = 0; i < cnt && !audible; i++)
                            {
                                IAudioSessionControl* pCtrl = nullptr;
                                if (FAILED(pSess->GetSession(i, &pCtrl)) || !pCtrl) continue;
                                AudioSessionState st;
                                if (SUCCEEDED(pCtrl->GetState(&st)) && st == AudioSessionStateActive)
                                {
                                    bool excluded = false;
                                    IAudioSessionControl2* pCtrl2 = nullptr;
                                    if (SUCCEEDED(pCtrl->QueryInterface(__uuidof(IAudioSessionControl2),
                                                                        (void**)&pCtrl2)) && pCtrl2)
                                    {
                                        // 系统通知会话（Notification 类）→ 排除。
                                        // 【S_FALSE 陷阱，§44 教训】IsSystemSoundsSession() 对"普通 App
                                        // 会话"返回 S_FALSE（不是 S_OK）；S_FALSE 的 severity bit=0，
                                        // SUCCEEDED(S_FALSE)==true。绝不能用 SUCCEEDED 判断——否则所有
                                        // App（Chrome/音乐）都被误判成系统通知而排除。必须 ==S_OK。
                                        if (pCtrl2->IsSystemSoundsSession() == S_OK) excluded = true;
                                        // 本进程自身会话（桌宠音效）→ 排除
                                        DWORD pid = 0;
                                        if (!excluded && SUCCEEDED(pCtrl2->GetProcessId(&pid)) &&
                                            pid == GetCurrentProcessId()) excluded = true;
                                        pCtrl2->Release();
                                    }
                                    if (!excluded)
                                    {
                                        IAudioMeterInformation* pMeter = nullptr;
                                        if (SUCCEEDED(pCtrl->QueryInterface(__uuidof(IAudioMeterInformation),
                                                                            (void**)&pMeter)) && pMeter)
                                        {
                                            float peak = 0.0f;
                                            if (SUCCEEDED(pMeter->GetPeakValue(&peak)) &&
                                                peak > kMusiconPeakEpsilon) audible = true;
                                            if (dbg) log("[输入] musicon 轮#%d 会话[%d] peak=%.4f -> %s",
                                                         polls, i, (double)peak, audible ? "出声!" : "未达阈");
                                            pMeter->Release();
                                        }
                                        else if (dbg) log("[输入] musicon 轮#%d 会话[%d] QI(Meter)失败", polls, i);
                                    }
                                    else if (dbg) log("[输入] musicon 轮#%d 会话[%d] Active 但被排除", polls, i);
                                }
                                pCtrl->Release();
                            }
                        }
                        pSess->Release();
                    }
                    pMgr->Release();
                }
                pDev->Release();
            }
            pEnum->Release();
        }
        // 心跳2：前 3 轮打印枚举摘要（定位线程是否真的在轮询/看到会话）
        if (++polls <= 3)
            log("[输入] musicon 心跳#%d: 会话数=%d 外部出声=%d hr=0x%08X",
                polls, sessCount, audible ? 1 : 0, hr);
        // 【出声保持 hangover + 快启慢放】根因（日志实锤）：播放期间 peak 反复跌破
        // 阈值（音乐间奏/安静段），musicon 每几秒翻转一次 → SM 反复进出"音乐状态"
        // → click/istyping 动画被反复打断卡死（用户实测"放音乐就乱"）。修复：
        // 一旦出声记 lastAudibleTick，kMusiconHoldMs 窗口内即使 peak 跌破也保持"出声"
        // （VAD 尾音保持标准做法，间奏不翻转）；窗口外才走快启慢放去抖（开始 2 次
        // =500ms 快启，停止 12 次=3s 慢放）。
        static DWORD lastAudibleTick = 0;
        if (audible) lastAudibleTick = GetTickCount();
        const bool audibleHold = audible ||
            (GetTickCount() - lastAudibleTick) < kMusiconHoldMs;
        static constexpr int kMusiconOnSamples = 2;
        static constexpr int kMusiconOffSamples = 12;
        if (audibleHold == last) stable++;
        else { last = audibleHold; stable = 1; }
        if (stable > 200) stable = 200;
        const int need = audibleHold ? kMusiconOnSamples : kMusiconOffSamples;
        if (stable >= need)
        {
            const bool want = last;
            const bool reported = InterlockedCompareExchange(&g_musiconState, 0, 0) != 0;
            if (want != reported)
            {
                InterlockedExchange(&g_musiconState, want ? 1 : 0);
                log("[输入] musicon → %s（%s）", want ? "true" : "false",
                    want ? "检测到外部音乐/媒体正在出声" : "无外部出声");
            }
        }
        // 枚举失败告警（限频 5s；失败时保持旧状态——默认 false 更稳妥）
        if (FAILED(hr) && GetTickCount() - lastWarnTick > 5000)
        {
            lastWarnTick = GetTickCount();
            log("[输入] musicon 检测失败 HRESULT=0x%08X（会话枚举不可用，保持当前状态）", hr);
        }
        Sleep(kMusiconPollMs);
    }
    CoUninitialize();
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    // 白边诊断模式（--diag-edge）：隐藏窗口渲染采样后自动退出，不打扰用户屏幕
    for (int i = 1; i < __argc; i++)
    {
        if (__wargv[i] && wcsstr(__wargv[i], L"--diag-edge") != nullptr)
            g_diagEdge = true;
    }
    // 产品化：WIN32 子系统（/SUBSYSTEM:WINDOWS）——启动不再弹黑色控制台窗口
    // （控制台子系统 main + FreeConsole 会先闪黑框，用户不接受）。日志走文件
    // g_logFile；无控制台时 stderr 句柄无效但无害（GUI 子系统下 vfprintf 返回负值）。
    InitializeCriticalSection(&g_logLock);
    g_logLockReady = true;
    // 多开：去掉 FindWindow 单实例退出——重复拉起即再开一只。
    // 槽位锁 runtime\pet.lock.<n> 记录 PID：活槽=PID 进程存在；死锁复用，全活追加。
    g_petSlot = acquirePetSlot();
    // 产品化：日志落文件（无控制台重定向时也留痕）。不用 ccs=UTF-8——
    // 该模式打开失败会静默返回 NULL（实测 BOM 留下、内容全丢，§16.18）；
    // 用二进制追加 + /utf-8 编译的 UTF-8 字节直写（fprintf 原样落盘）。
    {
        const std::wstring base = knownLocalAppData();
        if (!base.empty()) {
            const std::wstring logs = base + L"\\rime_beautify\\logs";
            CreateDirectoryW(logs.c_str(), nullptr);
            // 日志轮转：当前文件 >10MB → 删旧 .1 → 当前改名 .1 → 开新文件。
            // 磁盘上限≈20MB/份（.1 保留上次历史，下次轮转被删）。
            const std::wstring logPath = logs + L"\\pet_preview.log";
            const std::wstring logPath1 = logs + L"\\pet_preview.log.1";
            std::error_code lec;
            if (std::filesystem::file_size(logPath, lec) > 10LL * 1024 * 1024) {
                std::filesystem::remove(logPath1, lec);
                std::filesystem::rename(logPath, logPath1, lec);
            }
            g_logFile = _wfopen(logPath.c_str(), L"ab");
            if (g_logFile) log("[OK] 日志文件: %s\\pet_preview.log", wideToUtf8(logs).c_str());
        }
    }
    // 【§51 DPI 感知修复】必须在任何窗口/坐标 API 前调用：进程 DPI-unaware 时
    // GetWindowRect/GetCursorPos 是虚拟化逻辑坐标、WH_MOUSE_LL 钩子 ms->pt 是物理
    // 坐标，两者直接比较在 DPI>100% 屏误判菜单点击（右键菜单"能显示但交互不了"）。
    enableDpiAwareness();
    log("[多开] 本实例槽位=%d（配置 pet_config.%d.json）", g_petSlot, g_petSlot);
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    { log("[FAIL] WSAStartup"); return 1; }
    InitializeCriticalSection(&g_hudLock);
    Gdiplus::GdiplusStartupInput gsi;
    g_gdipStarted = (Gdiplus::GdiplusStartup(&g_gdipToken, &gsi, nullptr) == Gdiplus::Ok);
    if (g_gdipStarted) log("[OK] GDI+ 启动（按键 HUD 卡片）");
    if (__argc >= 2)
        g_defaultSkinPath = __wargv[1];
    else
    {
        // 便携版无参双击即用：默认皮肤 = exe 同目录默认 riv（kDefaultRivName）
        wchar_t mod[MAX_PATH];
        if (GetModuleFileNameW(nullptr, mod, MAX_PATH) == 0) mod[0] = L'\0';
        std::wstring dir(mod);
        size_t sep = dir.find_last_of(L'\\');
        if (sep != std::wstring::npos) dir = dir.substr(0, sep);
        g_defaultSkinPath = dir + L"\\" + kDefaultRivName;
        log("[皮肤] 无参数启动，默认皮肤 = exe 同目录 %s", "beautype_pet.riv");
    }
    // 热启动皮肤：config.skin 非空且皮肤库文件可读 → 用它；否则回退出厂默认（自愈）
    // 诊断模式强制默认皮肤（用户要求用 beautype_pet.riv 复现白边/锯齿），跳过热启动皮肤恢复
    if (!g_diagEdge)
        loadSkinNameFromConfig();
    // 热启动窗口：恢复上次的缩放比例 + 桌面位置（无效回退默认右下角）
    loadWindowStateFromConfig();
    std::wstring rivPath = g_defaultSkinPath;
    if (!g_skinName.empty())
    {
        const std::wstring sp = petSkinPath(g_skinName);
        std::ifstream probe(sp, std::ios::binary);
        if (probe)
        {
            rivPath = sp;
            log("[皮肤] 热启动恢复皮肤: %s", g_skinName.c_str());
        }
        else
        {
            log("[皮肤] 配置皮肤 %s 不存在，回退出厂默认", g_skinName.c_str());
            g_skinName.clear(); // 自愈：下次 savePetConfig 落空
        }
    }
    if (g_skinName.empty())
        log("[皮肤] 出厂默认皮肤: %s", wideToUtf8(rivPath).c_str());

    // ---- 读文件（__wargv：宽字符路径，中文/空格路径不乱码）----
    std::ifstream f(rivPath.c_str(), std::ios::binary | std::ios::ate);
    if (!f) { log("无法打开文件: %s", wideToUtf8(rivPath).c_str()); DeleteCriticalSection(&g_logLock); return 1; }
    auto size = f.tellg();
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(bytes.data()), size);
    f.close();
    log("[OK] riv 读取 %zu 字节", bytes.size());

    // ---- D3D11 ----
    D3D_FEATURE_LEVEL fl;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        &dev, &fl, &ctx);
    if (FAILED(hr)) { log("[FAIL] D3D11CreateDevice 0x%08X", hr); return 1; }
    g_dev = dev; g_ctx = ctx;
    log("[OK] D3D11 设备 feature level 0x%X", fl);

    g_rc = rive::gpu::RenderContextD3DImpl::MakeContext(g_dev, g_ctx, rive::gpu::D3DContextOptions{}).release();
    if (!g_rc) { log("[FAIL] MakeContext"); return 1; }
    log("[步进] MakeContext OK");

    // ---- 窗口 + 渲染资源（先于 riv 加载：加载期间窗口显示 DotsJump 过场动画，
    //      不再"空白很久"；加载链整体移到后台线程 petLoadThread）----
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = PET_WIN_CLASS;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    log("[步进] 窗口类注册 OK");

    if (!createRenderResources(g_winSize)) { DeleteCriticalSection(&g_logLock); return 1; }
    log("[步进] 渲染资源 OK");

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.right - (int)g_winSize - PET_MARGIN;
    int y = work.bottom - (int)g_winSize - PET_MARGIN;
    if (g_restoreX >= 0 && g_restoreY >= 0) // 热启动：恢复上次桌面位置
    {
        x = g_restoreX;
        y = g_restoreY;
        log("[配置] 恢复窗口位置 (%d,%d)", x, y);
    }
    else
    {
        const int stagger = countLivePetSlots() * 80;   // 多开错开：避免叠在同一只位置
        x -= stagger;
        y -= stagger;
    }
    // 【§48 窗口屏幕外修复】恢复位置/多开错开可能把窗口推到屏幕外（用户"双击没反应"，
    // 且退出时坏位置被持久化 → 永久屏外）。中心点不在任一显示器工作区 → 回退默认右下角。
    if (!windowCenterOnAnyScreen(x, y, (int)g_winSize, (int)g_winSize))
    {
        log("[配置] 窗口位置 (%d,%d) 不在屏幕内 → 回退默认右下角（§48 屏幕外修复）", x, y);
        x = work.right - (int)g_winSize - PET_MARGIN;
        y = work.bottom - (int)g_winSize - PET_MARGIN;
        g_restoreX = g_restoreY = -1;   // 坏位置不参与后续热启动
    }
    // 【根因 16.26】桌宠主窗口必须 WS_EX_NOACTIVATE：点击激活会抢键盘焦点 →
    // TSF 把输入法(beautype_tsf.dll)加载进桌宠进程 → 打字时 use-after-free 写穿堆
    // → 打字卡/黑底/0xc0000374 崩溃。设计上输入法只通过 IPC 通知桌宠，绝不进桌宠进程。
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        PET_WIN_CLASS, L"Beautype Pet", WS_POPUP, x, y, (int)g_winSize, (int)g_winSize,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) { log("[FAIL] CreateWindowExW"); DeleteCriticalSection(&g_logLock); return 1; }
    setPassthrough(true); // 默认穿透，鼠标悬停才可交互
    log("[步进] 窗口创建 OK (%d,%d)", x, y);

    SetTimer(g_hwnd, 1, PET_FPS_MS, nullptr);
    if (g_diagEdge) ShowWindow(g_hwnd, SW_HIDE);
    else ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(g_hwnd);
    g_loadingStart = GetTickCount(); // 加载过场动画从窗口显示起算
    renderFrame(); // 首帧 = DotsJump 加载过场动画
    log("[步进] 首帧（加载过场）渲染 OK");
    log("[OK] 桌宠窗口已显示于右下角 (%d,%d) %ux%u：悬停可交互/滚轮缩放/拖动/右键退出",
        x, y, g_winSize, g_winSize);

    // ---- 启动 riv 加载线程（import/artboard/状态机/音频，纯 CPU）----
    PetLoadArgs* la = new PetLoadArgs();
    la->bytes = std::move(bytes);
    // 支持第二个 CLI 参数指定初始状态机索引（调试用：pet_preview <riv> [sm_index]）
    if (__argc > 2)
    {
        try { la->smIndex = std::stoi(std::wstring(__wargv[2])); }
        catch (...) {}
    }
    // 第三/四参数: 自动切换调试（秒数 + 目标SM索引）
    if (__argc > 3)
    {
        try { la->autoSwitchSec = (UINT)std::stoi(std::wstring(__wargv[3])); } catch (...) {}
    }
    if (__argc > 4)
    {
        try { la->autoSwitchTarget = (UINT)std::stoi(std::wstring(__wargv[4])); } catch (...) {}
    }
    HANDLE hLoad = CreateThread(nullptr, 0, petLoadThread, la, 0, nullptr);
    if (!hLoad)
    {
        log("[FAIL] 启动加载线程");
        delete la;
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
    else
    {
        log("[步进] 加载线程已启动（riv 解析中，窗口显示过场动画）");
    }

    // 启动输入法事件链路（TCP 客户端：读端口文件→连接→hello 鉴权→收 key_event）
    // 线程句柄保留到清理：退出时须等 IPC 线程发完 bye 再结束进程（否则 bye 被截断）
    HANDLE hIpc = CreateThread(nullptr, 0, petIpcThread, nullptr, 0, nullptr);
    if (!hIpc) log("[FAIL] 启动 IPC 线程");

    // M4-1：全局低级键盘钩子（无 Beautype 输入法时兜底驱动 HUD/istyping）
    g_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, kbdHookProc, GetModuleHandleW(nullptr), 0);
    if (g_kbdHook) log("[输入] 全局键盘钩子已安装（无输入法时兜底）");
    else log("[输入] 键盘钩子安装失败（错误 %lu），仅输入法链路可用", GetLastError());

    // M4-1b：全局低级鼠标钩子（click 输入：任意左键按下 → 点击脉冲）
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseHookProc, GetModuleHandleW(nullptr), 0);
    if (g_mouseHook) log("[输入] 全局鼠标钩子已安装（click 输入）");
    else log("[输入] 鼠标钩子安装失败（错误 %lu）", GetLastError());

    // M4-2：musicon 后台检测线程（WASAPI 会话级"正在播放"，500ms 去抖；
    // 退出由 g_exit 通知，主循环结束后 Join 收尾）
    g_musiconThread = CreateThread(nullptr, 0, musiconThreadProc, nullptr, 0, nullptr);
    if (g_musiconThread) log("[输入] musicon 检测线程已启动（WASAPI 会话级，500ms 去抖）");
    else log("[输入] musicon 检测线程启动失败（错误 %lu），musicon 恒 false", GetLastError());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (g_exit) break;
        if (g_diagEdge && ++g_diagEdgeFrames >= 90)  // 90 帧≈3s 采样
        {
            log("[诊断] 白边像素采样完成，退出");
            PostQuitMessage(0);
        }
    }

    // 清理
    KillTimer(g_hwnd, 1);
    g_exit = true; // 通知 IPC 线程退出
    if (g_kbdHook) { UnhookWindowsHookEx(g_kbdHook); g_kbdHook = nullptr; }
    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    // 等加载线程收尾（若仍在跑，其仍持有 g_rc/g_dev；必须在销毁渲染资源前结束）
    if (hLoad) {
        WaitForSingleObject(hLoad, 30000);
        CloseHandle(hLoad);
    }
    // 用户主动退出 → 主线程独立连接发 bye（IPC 线程可能阻塞在 recv，靠不住）
    sendByeFromMain();
    if (hIpc) {
        WaitForSingleObject(hIpc, 2000);
        CloseHandle(hIpc);
    }
    if (g_musiconThread) {
        WaitForSingleObject(g_musiconThread, 2000); // g_exit 已置位，线程 250ms 内退出
        CloseHandle(g_musiconThread);
        g_musiconThread = nullptr;
    }
    if (g_engine) g_engine->stop();
    destroyRenderResources();
    delete g_rc;
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    if (g_gdipStarted) Gdiplus::GdiplusShutdown(g_gdipToken);
    DeleteCriticalSection(&g_hudLock);
    DeleteCriticalSection(&g_logLock);
    WSACleanup();
    if (g_logFile) { fclose(g_logFile); g_logFile = nullptr; }
    log("[OK] 桌宠已退出");
    return 0;
}
