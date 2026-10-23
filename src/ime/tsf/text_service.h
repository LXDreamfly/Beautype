// M1：真实 TSF 文本服务（从 spike ① 探针升级）。
// ActivateEx：建文档管理器 + 上下文入栈 + 全套 sink（键盘/线程焦点/文档编辑/
// 线程管理器事件/组合终止/显示属性提供者）；
// OnKeyDown：按键 → librime 引擎 → 行内组合（ITfComposition + 显示属性）/
// 上屏文本插入当前上下文 / 自绘候选窗随光标定位。
// 结构对齐小狼毫 WeaselTSF：一个服务类实现全部 TSF 接口族。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <windows.h>
#include <msctf.h>

// 官方 WeaselIPC.h 内联函数有未引用参数（C4100）；rime_tsf 以 /W4 /WX 编译，
// 需局部抑制（官方头本身不改动）
#pragma warning(push)
#pragma warning(disable : 4100)
#include <WeaselIPC.h>
#include <ResponseParser.h>
#pragma warning(pop)

#include "skin/theme.h"
#include "ui/candidate_window.h"
#include "tsf/candidate_list.h"
#include "tsf/language_bar.h"

namespace ime {

// 组合文本显示属性 GUID（ITfDisplayAttributeProvider 提供；TSF 以 guid atom
// 关联到 ITfRange 属性，实现行内拼音下划线——对齐 weasel/微软 TSF Sample）。
inline const GUID kGuidCompositionDisplayAttribute = {
    0xA1A3A0D4, 0xC0B0, 0x4F7B, {0x8C, 0xE4, 0x2C, 0x8B, 0x9C, 0x6D, 0x8F, 0x25}};

// 注意：ITfTextInputProcessorEx 已继承 ITfTextInputProcessor，此处只继承 Ex
//（重复继承会触发 C4584 且被 WX 当作错误）。QI 中两个 IID 都返回 Ex 指针。
class RimeTextService final : public ITfTextInputProcessorEx,
                              public ITfSource,
                              public ITfKeyEventSink,
                              public ITfThreadMgrEventSink,
                              public ITfTextEditSink,
                              public ITfTextLayoutSink,
                              public ITfThreadFocusSink,
                              public ITfCompositionSink,
                              public ITfDisplayAttributeProvider,
                              public ITfCompartmentEventSink,
                              public ITfActiveLanguageProfileNotifySink {
 public:
  // 服务化：DLL 不再持有本地 librime 引擎，按键经官方 Client（IPC）转发给
  // rime_server.exe（服务进程内引擎 + 候选窗 + 托盘）。
  explicit RimeTextService();
  ~RimeTextService() = default;

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // ITfTextInputProcessor / ITfTextInputProcessorEx（对齐 weasel：Activate 转
  // ActivateEx，全套初始化在 ActivateEx 完成）
  STDMETHODIMP Activate(ITfThreadMgr* pThreadMgr, TfClientId tfClientId) override;
  STDMETHODIMP ActivateEx(ITfThreadMgr* pThreadMgr, TfClientId tfClientId,
                          DWORD dwFlags) override;
  STDMETHODIMP Deactivate() override;

  // ITfKeyEventSink 与 ITfThreadMgrEventSink 各有一个 OnSetFocus（签名不同）：
  // 显式引入，避免派生类中的名字隐藏导致调用歧义
  using ITfKeyEventSink::OnSetFocus;
  using ITfThreadMgrEventSink::OnSetFocus;

  // ITfSource（ITfKeystrokeMgr::AdviseKeyEventSink 握手）
  STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD* pdwCookie) override;
  STDMETHODIMP UnadviseSink(DWORD dwCookie) override;

  // ITfKeyEventSink
  STDMETHODIMP OnSetFocus(BOOL fForeground) override;
  STDMETHODIMP OnTestKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                             BOOL* pfEaten) override;
  STDMETHODIMP OnKeyDown(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                         BOOL* pfEaten) override;
  STDMETHODIMP OnTestKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                           BOOL* pfEaten) override;
  STDMETHODIMP OnKeyUp(ITfContext* pic, WPARAM wParam, LPARAM lParam,
                       BOOL* pfEaten) override;
  STDMETHODIMP OnPreservedKey(ITfContext* pic, REFGUID rguid, BOOL* pfEaten) override;

  // ITfThreadMgrEventSink（文档管理器生命周期跟踪，对齐 weasel _InitThreadMgrEventSink）
  STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pDocMgr) override;
  STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pDocMgr) override;
  STDMETHODIMP OnSetFocus(ITfDocumentMgr* pDocMgrFocus,
                          ITfDocumentMgr* pDocMgrPrevFocus) override;
  STDMETHODIMP OnPushContext(ITfContext* pContext) override;
  STDMETHODIMP OnPopContext(ITfContext* pContext) override;

  // ITfTextEditSink（文档被外部修改时中止组合；本服务发起的编辑忽略）
  STDMETHODIMP OnEndEdit(ITfContext* pContext, TfEditCookie ecRead,
                         ITfEditRecord* pEditRecord) override;

  // ITfTextLayoutSink（布局变化 → 候选窗重新定位）
  STDMETHODIMP OnLayoutChange(ITfContext* pContext, TfLayoutCode lcode,
                              ITfContextView* pContextView) override;

  // ITfCompartmentEventSink：监听系统/用户点击中英指示器导致的 conversion
  // compartment 变化（对齐 weasel Compartment.cpp），同步引擎 ascii_mode
  STDMETHODIMP OnChange(REFGUID guidCompartment) override;

  // ITfThreadFocusSink（线程失焦 → 中止组合，对齐 weasel OnKillThreadFocus）
  STDMETHODIMP OnSetThreadFocus() override;
  STDMETHODIMP OnKillThreadFocus() override;

  // ITfActiveLanguageProfileNotifySink（对齐 weasel OnActivated：
  // 激活 → 语言栏状态刷新；停用 → 销毁候选）
  STDMETHODIMP OnActivated(REFCLSID clsid, REFGUID guidProfile,
                           BOOL isActivated) override;

  // ITfCompositionSink（组合被宿主/系统终止时回调）
  STDMETHODIMP OnCompositionTerminated(TfEditCookie ecWrite,
                                       ITfComposition* pComposition) override;

  // ITfDisplayAttributeProvider（行内组合下划线样式）
  STDMETHODIMP EnumDisplayAttributeInfo(
      IEnumTfDisplayAttributeInfo** ppEnum) override;
  STDMETHODIMP GetDisplayAttributeInfo(REFGUID guid,
                                       ITfDisplayAttributeInfo** ppInfo) override;

  bool activated() const { return activated_; }
  std::uint64_t session() const { return session_; }
  std::string last_commit() const { return last_commit_; }

  // 服务化测试访问器：最近一次 IPC 响应的组合串/候选/状态（服务进程内引擎
  // 计算后回传，DLL 侧不再有本地 librime 引擎）。测试断言经此读取，避免
  // 旧架构的本地 engine.GetInput/GetCandidates 断言（服务化后恒为空）。
  const weasel::Context& ipc_context() const { return ipc_context_; }
  const weasel::Status& ipc_status() const { return ipc_status_; }
  bool client_connected() const { return client_connected_; }

  // 服务端候选窗锚定：ServerPositionEditSession（官方 CGetTextExtentEditSession
  // 语义，Composition.cpp:159-245）在 EditSession 内取组合区屏幕坐标后回调
  void ReportServerPosition(const RECT& rc);

  // 演示/验收用：候选窗锚定到 anchor 窗口右侧（系统级随光标定位属 M1.1）
  void SetCandidateWindowAnchor(HWND anchor);

  // 候选窗皮肤主题（不注入时按默认主题渲染，保证演示路径候选可见）
  void SetTheme(const skin::Theme& theme);

  // 皮肤图片目录（theme.yaml 目录）：传给候选窗解析相对路径 PNG 皮肤
  void SetConfigDir(const std::string& config_dir);

  // 演示宿主：上屏回显回调（M1 演示路径：commit 插入演示文本框；真实 TSF 路径不设置，保持无副作用）
  void SetCommitCallback(std::function<void(const std::string&)> cb);

  // 统一按键入口（演示宿主用）：vk 为 WM_KEYDOWN 的 wParam（VK 码），
  // mask 为 ime::KeyMask 位组合。修饰键状态由调用方在按键发生时刻捕获传入。
  // TSF 真实路径（OnKeyDown）同步送达，仍自行查询修饰键后走本入口。
  void HandleRawKey(int vk, int mask, BOOL* pfEaten = nullptr);

  // TSF 完整按键入口（对齐官方 KeyEventSink.cpp _ProcessKeyEvent：down/up 都
  // 进引擎）：lparam 提供 scan 码/扩展位（区分左右 Shift/Ctrl），is_up=true 时
  // mask 加 kKeyMaskRelease 并执行官方 CapsLock 取消大写锁定模拟
  void HandleRawKeyEx(int vk, int mask, LPARAM lparam, bool is_up,
                      BOOL* pfEaten = nullptr);

  // 演示宿主：按键释放事件（标准 OnKeyUp 语义 = 不消费，仅记录）
  void HandleKeyUp(int vk, int mask);

  // LL 键盘钩子回调同步路径：只做纯引擎计算，返回是否吃键；commit 经
  // commit_out 回传但不应用。调用方回到主线程后必须调 ApplyKeyResult()。
  bool ProcessKeySync(int vk, int mask, std::string* commit_out);

  // 应用上一次 ProcessKeySync 的结果（主线程调用）
  void ApplyKeyResult();

  // Ctrl+空格：切换中英（ascii_mode）
  void ToggleAsciiMode();
  // 设置中英模式（语言栏左键/右键菜单调用）
  void SetAsciiMode(bool on);
  // F4：显示方案选择菜单
  void ShowSchemaMenu();

  // ---- P2：语言栏 + 状态 compartment（对齐 weasel LanguageBar.cpp）----
  // 语言栏按钮注册/注销（ActivateEx/Deactivate 调用）
  bool InitLanguageBar();
  void UninitLanguageBar();
  // 写 GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION（中/英 + 全半角位）
  void UpdateLanguageBarCompartment();
  // 通用 compartment 读写（对齐 weasel Compartment.cpp）
  bool GetCompartmentDWORD(DWORD& value, REFGUID guid);
  bool SetCompartmentDWORD(DWORD value, REFGUID guid);

  // ---- 行内组合（供内部编辑会话类调用，故置于 public）----
  // 记录组合创建结果（StartCompositionEditSession 回调内调用）
  void SetComposition(ITfComposition* pComposition);
  // StartComposition 成功后立即触发定位（weasel CStartCompositionEditSession
  // 末尾调 _UpdateCompositionWindow）
  void OnCompositionStarted(ITfContext* ctx);
  // 释放组合引用（EndComposition/CommitComposition 回调内先释放再
  // EndComposition，避免同步回调 OnCompositionTerminated 误判外部中止——
  // 对齐 weasel _FinalizeComposition）
  void FinalizeComposition();
  // 更新行内预编辑文本 + 光标（preedit 为组合文本，sel_cursor<0 光标在末尾）
  void UpdateComposition(ITfContext* ctx, const std::wstring& preedit,
                         int sel_cursor);
  // 组合显示属性（下划线）
  void SetCompositionDisplayAttributes(TfEditCookie ec, ITfContext* ctx,
                                       ITfRange* range);
  void ClearCompositionDisplayAttributes(TfEditCookie ec, ITfContext* ctx,
                                       ITfRange* range);

 private:
  // 候选窗用户交互接线（点击选词 / 滚轮翻页 → 引擎动作）
  void SetupCandidateInteractions();
  void OnCandidateClicked(int index);
  void OnCandidateScroll(bool up);
  // 会话就绪保证（Client 连接 + 会话）
  bool EnsureSession();
  // 转发按键到服务端并解析响应（官方 ProcessKeyEvent + GetResponseData）
  bool SendKeyToServer(int keycode, int mask);
  // 按键处理主路径：Client 转发 → 提交/组合/位置上报
  void HandleKey(int keycode, int mask, BOOL* pfEaten);
  // 上屏：组合存在时替换组合文本并结束组合（TSF 标准提交），否则直接插入
  void CommitComposition(const std::wstring& commit);
  void InsertCommitText(const std::string& commit);
  // 刷新行内预编辑（composition）+ 上报输入位置（服务端弹候选窗）
  void UpdateCandidateWindow();
  // 只做轻量重新定位（OnLayoutChange 专用）：复用只读 ServerPositionEditSession
  // 上报输入位置，不写文本/不 StartComposition——写文本会再触发 OnLayoutChange
  // 形成无限自激循环（2026-10-07 真机卡死根因，对齐 weasel _UpdateCompositionWindow）
  void RepositionCandidateWindow();

  // ---- 行内组合（对齐 weasel Composition.cpp）----
  // 启动组合：编辑会话内 InsertTextAtSelection(QUERYONLY) 取 range →
  // ITfContextComposition::StartComposition → 光标到组合末尾 + 显示属性
  void StartComposition(ITfContext* ctx);
  // 结束组合：clear=true 时清空组合文本（取消）；clear=false 时保留已写入
  // 的提交文本（上屏提交路径）
  void EndComposition(bool clear);
  // 中止组合（外部终止/失焦/切换）：清空文本并释放
  void AbortComposition();

  // ---- 生命周期 sink 初始化（对齐 weasel ActivateEx 顺序）----
  bool InitThreadMgrEventSink();
  void UninitThreadMgrEventSink();
  bool InitTextEditSink(ITfDocumentMgr* pDocMgrFocus);
  void UninitTextEditSink();
  bool InitTextLayoutSink(ITfDocumentMgr* pDocMgrFocus);
  void UninitTextLayoutSink();
  bool InitThreadFocusSink();
  void UninitThreadFocusSink();
  // ITfActiveLanguageProfileNotifySink（对齐 weasel OnActivated）
  bool InitActiveLanguageProfileNotifySink();
  void UninitActiveLanguageProfileNotifySink();
  bool InitDisplayAttribute();
  // 监听 GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION（weasel Compartment.cpp）
  bool InitCompartment();
  void UninitCompartment();
  void UninitDisplayAttribute();
  // 键盘开关 compartment（GUID_COMPARTMENT_KEYBOARD_OPENCLOSE）
  bool IsKeyboardOpen();
  HRESULT SetKeyboardOpen(bool open);
  // 系统键盘禁用检查（对齐官方 Compartment.cpp:90-142 _IsKeyboardDisabled：
  // 焦点上下文 GUID_COMPARTMENT_KEYBOARD_DISABLED / EMPTYCONTEXT）
  bool IsKeyboardDisabled();
  // CapsLock 补键（对齐 weasel：caps 键传给引擎但永不吃键，系统完成切换）
  void ProcessCapsLockKey(int mask);

  // 服务化 Client（官方 IPC）：DLL 不加载 librime，按键/候选/状态全走服务进程
  weasel::Client client_;
  // 官方 ResponseParser 需外部持有 commit/Context/Status（WeaselIPCData.h 结构）
  // 注意：Config/UIStyle 也必须传入——官方 Configurator 解析 config.* 行时
  // 直接解引用 p_config（Configurator.cpp:21），传 nullptr 会访问冲突。
  std::wstring last_commit_wide_;
  weasel::Context ipc_context_;
  weasel::Status ipc_status_;
  weasel::Config ipc_config_;
  weasel::UIStyle ipc_style_;
  weasel::ResponseParser parser_{&last_commit_wide_, &ipc_context_, &ipc_status_,
                                  &ipc_config_, &ipc_style_};
  bool client_connected_ = false;
  bool session_started_ = false;
  bool pending_ime_on_focus_ = false;  // OnSetFocus(TRUE) 时 IPC 未连，补发标记
  bool pending_focus_in_ = false;  // OnSetFocus(TRUE) 时 IPC 未连，补发 FocusIn 标记
                                   // （2026-10-17：last_active_session_ 恒 0 导致
                                   // 服务端候选点击 SelectCandidateOnCurrentPage(index,0)
                                   // 直接 return，候选从未真正 select——点击不上屏根因）
  // 响应缓存（服务端回传）
  bool last_ascii_mode_ = false;
  volatile ULONG ref_count_ = 1;
  DWORD compartment_sink_cookie_ = TF_INVALID_COOKIE;
  bool activated_ = false;
  bool key_sink_advised_ = false;
  DWORD sink_cookie_next_ = 1;
  // 对齐小狼毫 KeyEventSink.cpp 的 _fTestKeyDownPending
  BOOL f_test_key_down_pending_ = FALSE;
  // 对齐官方 _fTestKeyUpPending：OnTestKeyUp 发送 keyup 后置位，OnKeyUp 去重
  //（官方 KeyEventSink.cpp:117-147，防 Word 等应用重复发 OnTestKeyUp）
  BOOL f_test_key_up_pending_ = FALSE;
  // 对齐官方 KeyEventSink.cpp:7-9（static prevKeyEvent/prevfEaten/keyCountToSimulate）：
  // CapsLock 取消大写锁定模拟（官方 40-58 行）——CapsLock 按下被吃 + 释放 +
  // caps 亮 + 已上屏/组合中 → SendInput 补按 CapsLock 取消大写锁定
  int key_count_to_simulate_ = 0;
  bool prev_key_eaten_ = false;
  int prev_key_keycode_ = 0;
  // 最近一次按键处理是否产生上屏（对齐官方 _committed 语义）
  bool last_key_had_commit_ = false;
  // Shift 单独按下（无其他键组合）→ OnKeyUp 时切换中英（weasel 输入习惯）
  bool shift_press_pending_ = false;
  // 2026-10-10 渲染风暴修复：上次上报的输入位置（ReportServerPosition 去重——
  // TSF OnLayoutChange 在组合文本更新时会多次触发，单键日志显示 20+ 次相同 rc
  // 上报 → 服务端 SetPosition 反复入队 → 候选窗渲染风暴 → 输入框右侧打字卡顿。
  // 微软输入法无自绘候选窗架构故无此问题）。上次组合状态（UpdateCandidateWindow
  // preedit 幂等：composing+preedit 未变则跳过 UpdateComposition 写文档——写文本
  // 会再触发 OnLayoutChange → 自激上报循环）。
  RECT last_reported_rc_{0, 0, 0, 0};
  // 2026-10-23 会话维度去重：去重基准所属会话（服务端 sid）。失焦→IPC 重连→
  // 服务端重建会话（sid 变化），新会话首次位置必须上报，否则服务端 haspos=0
  // → 候选窗空窗（真机 06:31:17-41 复现"重试多次只有拼音"）。初始 ~0ull
  // 保证进程内首次上报必发。
  std::uint64_t last_reported_session_ = ~0ull;
  bool last_composing_ = false;
  std::wstring last_preedit_;
  // 2026-10-22 方向键光标修复：preedit 光标（wchar 前缀长度，-1=末尾）。
  // 方向键移动光标不改变 preedit 文本，刷新幂等判断必须把光标纳入比较，
  // 否则宿主组合光标永远停在末尾（"左右方向键不能移动拼音光标"）。
  int last_preedit_cursor_ = -1;
  TfClientId client_id_ = 0;
  DWORD activate_flags_ = 0;
  ITfThreadMgr* thread_mgr_ = nullptr;
  ITfDocumentMgr* document_mgr_ = nullptr;
  ITfContext* context_ = nullptr;
  // 行内组合
  ITfComposition* composition_ = nullptr;
  ITfContext* composition_context_ = nullptr;
  // weasel _pTextEditSinkContext：InitTextEditSink 时保存当前焦点上下文，
  // OnLayoutChange 比对——非本上下文的布局变化直接忽略（闸门2）。
  ITfContext* text_edit_sink_context_ = nullptr;
  // 生命周期 sink cookie
  DWORD thread_mgr_event_sink_cookie_ = TF_INVALID_COOKIE;
  DWORD text_edit_sink_cookie_ = TF_INVALID_COOKIE;
  DWORD text_layout_sink_cookie_ = TF_INVALID_COOKIE;
  DWORD thread_focus_sink_cookie_ = TF_INVALID_COOKIE;
  DWORD lang_profile_cookie_ = TF_INVALID_COOKIE;
  // 显示属性 guid atom
  TfGuidAtom display_attribute_guid_atom_ = TF_INVALID_GUIDATOM;
  std::uint64_t session_ = 0;
  std::string last_commit_;
  // 演示宿主回显回调（M1；真实 TSF 路径为 null）
  std::function<void(const std::string&)> commit_callback_;
  ui::CandidateWindow candidate_window_;
  // F4 方案菜单模式
  bool schema_menu_mode_ = false;
  std::vector<std::string> schema_menu_list_;
  // 中英模式：true = 英文模式（字母直接上屏）
  bool ascii_mode_on_ = false;
  // P2：UWP 集成候选 + 语言栏按钮
  ime::tsf::CandidateListUI* candidate_ui_ = nullptr;
  ime::tsf::LanguageBarButton* lang_bar_ = nullptr;
};

}  // namespace ime
