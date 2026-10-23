// M1：librime 引擎封装（C API 薄封装 + 生命周期管理）。
// 输入法跑通的最小切片：初始化/部署/会话/按键→候选→上屏 全部经由此类。
// 对外不暴露 rime_api.h（用 void* 持有），替换引擎实现时可不动调用方。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ime {

// main.cpp 打开的统一日志文件句柄（stdout + 文件双写用），跨翻译单元共享。
extern FILE* g_log;

// 修饰键位（与 librime key_table.h 的 RimeModifier 位序一致：
// kShiftMask=1<<0, kLockMask(CapsLock)=1<<1, kControlMask=1<<2, kMod1Mask(Alt)=1<<3。
// 真机根因 2026-10-05：此前 Ctrl=1<<1/Alt=1<<2 与 librime 错位——Ctrl 被认成
// CapsLock、Alt 被认成 Ctrl，导致 Ctrl+C/V 等组合键被引擎误吞、Alt 组合错乱。
// 对齐后修饰键位与 librime/weasel 完全一致。）
enum KeyMask : int {
  kKeyMaskShift = 1 << 0,     // librime kShiftMask
  kKeyMaskCapsLock = 1 << 1,  // librime kLockMask
  kKeyMaskControl = 1 << 2,   // librime kControlMask
  kKeyMaskAlt = 1 << 3,       // librime kMod1Mask (kAltMask)
  // 按键释放位：对齐官方 weasel include/KeyEvent.h（ibus RELEASE_MASK = 1<<14）。
  // weasel::KeyEvent.mask 是 UINT16，1<<14 是 16 位内可存的 release 位；服务端
  // BeautypeHandler 用官方 expand_ibus_modifier 展开成 librime kReleaseMask
  // （1<<30）后再 process_key（官方 RimeWithWeasel.cpp:264-292）。
  kKeyMaskRelease = 1 << 14,
};

struct Candidate {
  std::string text;
  std::string comment;
};

class RimeEngine {
 public:
  RimeEngine() = default;
  ~RimeEngine();

  RimeEngine(const RimeEngine&) = delete;
  RimeEngine& operator=(const RimeEngine&) = delete;

  // 初始化 + 同步部署。shared_data_dir 指向预置数据目录（librime 的 data/minimal，
  // 含 default.yaml + luna_pinyin schema/dict + essay），user_data_dir 为可写目录
  // （部署产物 build/ 写其中）。部署编译 .dict.yaml → .table.bin/.prism.bin，
  // 完成后即可建会话输入。
  bool Initialize(const std::string& shared_data_dir,
                  const std::string& user_data_dir,
                  const std::string& schema_id = "rime_ice");
  void Finalize();

  // 重新部署（照官方 WeaselDeployer /deploy 语义）：
  // deployer_initialize + deploy 重跑部署任务（重新编译词典/更新方案）。
  // 调用方须先 EnterMaintenance（部署期间引擎不处理按键）。
  bool Redeploy();

  bool initialized() const { return initialized_; }
  std::string version() const;

  // 会话管理
  std::uint64_t CreateSession();
  void DestroySession(std::uint64_t session);
  bool SelectSchema(std::uint64_t session, const std::string& schema_id);

  // 按键处理：keycode 为 VK 码（WM_KEYDOWN 的 wParam），mask 为 ime::KeyMask 组合。
  // 返回是否被输入法吞掉。
  bool ProcessKey(std::uint64_t session, int keycode, int mask);

  // 未读上屏文本（调用后清空该次提交缓冲）
  std::string GetCommit(std::uint64_t session);

  // 上下文快照
  std::string GetInput(std::uint64_t session);
  std::string GetCompositionText(std::uint64_t session);
  // preedit 光标位置（utf8 字节偏移，librime RimeComposition 的
  // sel_start/sel_end/cursor_pos）。服务端发 ctx.preedit.cursor 用
  // （官方 RimeWithWeasel.cpp:798-801/831-838 语义）；返回 false =
  // 无组合/取不到上下文。
  bool GetPreeditCursor(std::uint64_t session, int* sel_start, int* sel_end,
                        int* cursor_pos) const;
  std::vector<Candidate> GetCandidates(std::uint64_t session);
  // 当前高亮候选下标（左右键选词后随之变化；-1 = 无候选/无高亮）
  int GetHighlightedIndex(std::uint64_t session);
  // 当前输入方案 id（switcher 切换方案后随之变化，如 luna_pinyin/double_pinyin/wubi86）
  std::string GetCurrentSchema(std::uint64_t session);
  bool SelectCandidate(std::uint64_t session, std::size_t index);

  // 设置选项（ascii_mode 中英切换等）
  void SetOption(std::uint64_t session, const std::string& key, bool value);
  // 查询选项状态（ascii_mode / full_shape 等；服务化后 handler 用）
  bool GetOption(std::uint64_t session, const std::string& key);
  // 组合状态（官方 RimeStatus.is_composing；handler 响应 ctx/status 用）
  bool IsComposing(std::uint64_t session);
  // 清除组合（未上屏的拼音/已选词；对应 librime clear_composition）
  void ClearComposition(std::uint64_t session);
  // 提交当前组合（官方 librime RimeApi::commit_composition，rime_api.h:327）：
  // 把正在编辑的组合原文/译文整体上屏。供服务端修复「标点被吃进组合却不提交」
  // 场景（数字后句号第一下不上屏：librime punct 在空组合上下文未触发
  // AutoCommitPunct，标点卡在组合里；服务端检测到后调用本方法补上屏）。
  void CommitComposition(std::uint64_t session);
  // 获取所有可用 schema 列表
  std::vector<std::string> GetSchemaList();

 private:
  void* api_ = nullptr;  // RimeApi*
  bool initialized_ = false;
  std::string schema_id_;
  std::string log_dir_;  // librime 内部日志目录（user/log），生命周期须覆盖初始化期
  std::string shared_data_dir_;  // Redeploy 需要（deployer_initialize 的 traits）
  std::string user_data_dir_;
};

}  // namespace ime
