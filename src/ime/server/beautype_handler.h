// 服务进程 RequestHandler（照官方 weasel RequestHandler 接口实现）。
// 负责：会话管理 → 按键处理 → 引擎执行 → 候选窗刷新 → 官方行协议响应回传。
// 响应格式 100% 对齐官方 RimeWithWeasel（action 头 + ctx/status/config/commit 行，
// ctx.cand 用 boost text_woarchive 归档），DLL 端用官方 ResponseParser 解析。
#pragma once

#include <WeaselIPC.h>
#include <atomic>
#include <functional>
#include <map>
#include <string>

#include "engine/rime_engine.h"
#include "server/pet_push_server.h"
#include "ui/candidate_window.h"

namespace ime::server {

class BeautypeHandler : public weasel::RequestHandler {
 public:
  BeautypeHandler(ime::RimeEngine& engine, ime::ui::CandidateWindow& win);
  ~BeautypeHandler() override = default;

  // ---- weasel::RequestHandler 接口（官方 ServerImpl 回调） ----
  DWORD FindSession(DWORD session_id) override;
  DWORD AddSession(LPWSTR buffer, EatLine eat) override;
  DWORD RemoveSession(DWORD session_id) override;
  BOOL ProcessKeyEvent(weasel::KeyEvent keyEvent, DWORD session_id,
                       EatLine eat) override;
  void CommitComposition(DWORD session_id) override;
  void ClearComposition(DWORD session_id) override;
  void SelectCandidateOnCurrentPage(size_t index, DWORD session_id) override;
  bool HighlightCandidateOnCurrentPage(size_t index, DWORD session_id,
                                       EatLine eat) override;
  bool ChangePage(bool backward, DWORD session_id, EatLine eat) override;
  void FocusIn(DWORD param, DWORD session_id) override;
  void FocusOut(DWORD param, DWORD session_id) override;
  // M4+：输入法激活/停用（TSF OnActivated）→ 桌宠显隐（方案 A）
  void ImeActivated(bool activated, DWORD session_id) override;
  void UpdateInputPosition(RECT const& rc, DWORD session_id) override;
  void SetOption(DWORD session_id, const std::string& opt, bool val) override;
  void UpdateColorTheme(BOOL darkMode) override;

  // ---- 维护模式（照官方 RimeWithWeasel.cpp:475/481 StartMaintenance/EndMaintenance）----
  // 部署器/安装器运行时暂停输入（引擎可被部署线程独占写 user 数据）；
  // 维护结束后恢复。官方由 Deployer 经 IPC StartMaintenance 触发。
  void StartMaintenance() override;
  void EndMaintenance() override;
  bool maintenance() const { return maintenance_; }

  // 供托盘全局切换（遍历全部会话）
  void SetAsciiModeForAll(bool ascii);
  // 供托盘方案切换（遍历全部会话选方案）
  void SelectSchemaForAll(const std::string& schema_id);
  // 供候选窗点击/托盘读取状态
  DWORD last_session_id() const { return last_active_session_; }
  // 2026-10-17：候选窗点击/翻页改用候选窗所属会话（shown_session_，候选窗
  // Show 时记录）。last_active_session_ 依赖 TSF FocusIn，焦点事件在"输入法
  // 激活时焦点已定"的场景下不触发 → 恒 0 → 点击 SelectCandidateOnCurrentPage
  // (index, 0) 直接 return（真机日志 s=0，点击候选不上屏根因）。
  DWORD shown_session() const { return shown_session_; }
  void set_global_ascii_mode(bool on) { global_ascii_mode_ = on; }
  bool global_ascii_mode() const { return global_ascii_mode_; }

  // 桌宠事件推送（M3-4）：server_main 装配 PushServer 后注入；
  // ProcessKeyEvent 里按键转发（best-effort，桌宠不在线不影响输入法）。
  void set_pet_push(PetPushServer* push) { pet_push_ = push; }
  // M4+：首次切换输入法启动桌宠的拉起回调（server_main 注入 LaunchPetIfInstalled）
  void set_pet_launcher(std::function<void()> cb) { pet_launcher_ = std::move(cb); }
  // 维护期托盘图标回调（server_main 注入：开始→NIM_ADD 显示，结束→NIM_DELETE 收回；
  // 对齐官方 WeaselTrayIcon.cpp:82-90 disabled=true→ShowIcon）
  void set_maintenance_listener(std::function<void(bool)> cb) {
    maintenance_listener_ = std::move(cb);
  }
  // 用户右键退出桌宠（bye 帧回调设置）→ 本次服务生命周期不再自动拉起
  void SetPetUserExited() { pet_user_exited_ = true; }
  bool pet_user_exited() const { return pet_user_exited_; }

 private:
  // 会话 id 映射（官方 DWORD 会话 → RimeEngine 会话）
  std::uint64_t ToSession(DWORD session_id) const;
  // 用官方行协议构造响应（commit/ctx/status/config）并回传 DLL
  void SendResponse(DWORD session_id, EatLine eat);
  // 候选窗同步（候选/preedit/schema 标题）
  void SyncUi(DWORD session_id);

  ime::RimeEngine& engine_;
  ime::ui::CandidateWindow& win_;
  std::map<DWORD, std::uint64_t> sessions_;
  DWORD next_session_id_ = 1;
  DWORD last_active_session_ = 0;  // 最近聚焦会话（候选窗点击归属）
  bool global_ascii_mode_ = false; // 托盘中英状态（新建会话时应用）
  // 点选候选后 pending（VK_SELECT 模拟键触发响应时消费）
  bool select_pending_ = false;
  // 维护模式（部署器运行时暂停输入）；部署在工作线程执行，跨线程读 → atomic
  std::atomic<bool> maintenance_{false};

  // ---- 跨窗口候选窗串台修复（真机 2026-10-13） ----
  // 候选窗是服务端单例，但每个应用（会话）的光标锚点不同。此前 SyncUi 直接
  // 在"窗口当前坐标"（= 最近一次 UpdateInputPosition 的全局值，可能来自别的
  // 应用）上显示 → WPS→豆包切换后候选窗先驻留 WPS 坐标、再闪现回豆包。
  // 修复：按会话缓存各自的光标锚点；SyncUi 只用"本会话自己的坐标"显示；
  // 位置未到的会话先隐藏挂起，位置上报后补显示；焦点切换时收起他会话残留。
  std::map<DWORD, RECT> session_pos_;      // 各会话最近一次光标锚点（rc.left/bottom）
  std::map<DWORD, bool> session_has_pos_;  // 会话是否已上报过有效位置
  std::map<DWORD, bool> pending_show_;     // 会话候选就绪但位置未到（挂起待补）
  DWORD shown_session_ = 0;                // 当前可见候选窗内容归属的会话

  PetPushServer* pet_push_ = nullptr;      // 桌宠推送（M3-4；可空）
  std::function<void()> pet_launcher_;     // 首次激活拉起桌宠（M4+；可空）
  std::function<void(bool)> maintenance_listener_;  // 维护期图标回调（可空）
  std::atomic<bool> pet_user_exited_{false}; // 用户主动退出桌宠（bye；跨线程读）
};

}  // namespace ime::server
