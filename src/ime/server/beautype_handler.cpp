#include "server/beautype_handler.h"
#include "log/log.h"

#include <sstream>
#include <vector>

#include <boost/archive/text_woarchive.hpp>

#include <WeaselIPCData.h>
#include <WeaselUtility.h>

// 官方工具：UTF-8 → wstring（u8tow 在 WeaselUtility.h:245，CP_UTF8；宽字符转回
// 也用官方）。注意：不依赖 u8tow 宏，这里显式调用 MultiByteToWideChar(CP_UTF8)
// ——等价于官方 u8tow 展开，但绕开任何宏被头文件覆盖/未展开的意外，确保服务端
// 发送的 commit/preedit 一定是真 UTF-16 宽字符（Word 等应用中文字符乱码根因
// 排查 2026-10-07）。
static std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                nullptr, 0);
  if (len <= 0) return std::wstring();
  std::wstring res;
  res.resize(static_cast<std::size_t>(len));
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &res[0],
                      len);
  return res;
}

namespace ime::server {

using weasel::RequestHandler;

BeautypeHandler::BeautypeHandler(ime::RimeEngine& engine,
                                 ime::ui::CandidateWindow& win)
    : engine_(engine), win_(win) {}

std::uint64_t BeautypeHandler::ToSession(DWORD session_id) const {
  auto it = sessions_.find(session_id);
  return it == sessions_.end() ? 0 : it->second;
}

DWORD BeautypeHandler::FindSession(DWORD session_id) {
  return sessions_.count(session_id) ? session_id : 0;
}

DWORD BeautypeHandler::AddSession(LPWSTR buffer, EatLine eat) {
  const DWORD sid = next_session_id_++;
  const std::uint64_t rime_session = engine_.CreateSession();
  sessions_[sid] = rime_session;
  // 新会话继承托盘中英状态
  if (global_ascii_mode_)
    engine_.SetOption(rime_session, "ascii_mode", true);
  // 照官方 RimeWithWeasel.cpp:207-209：会话创建后写一次响应（status/ctx），
  // 客户端 StartSession 后的 GetResponseData 依赖它，否则读管道阻塞
  if (eat)
    SendResponse(sid, eat);
  return sid;
}

DWORD BeautypeHandler::RemoveSession(DWORD session_id) {
  auto it = sessions_.find(session_id);
  if (it == sessions_.end())
    return 0;
  engine_.DestroySession(it->second);
  sessions_.erase(it);
  session_pos_.erase(session_id);
  session_has_pos_.erase(session_id);
  pending_show_.erase(session_id);
  return 0;
}

void BeautypeHandler::SyncUi(DWORD session_id) {
  const auto s = ToSession(session_id);
  if (s == 0)
    return;
  const auto cands = engine_.GetCandidates(s);
  std::vector<std::string> texts;
  texts.reserve(cands.size());
  for (const auto& c : cands)
    texts.push_back(c.text);
  const int hl = engine_.GetHighlightedIndex(s);
  // 诊断日志（真机 2026-10-13）：SyncUi 由哪个会话触发、候选数、该会话是否
  // 有位置、当前可见归属——与 UpdateInputPosition 日志对读即可定位跨会话串台
  // full_shape=1 时 librime ShapeFormatter 会把 commit 的 ASCII 转全角
  // （无候选回车上屏拼音变全角=输入框"变宽"，官方 shape.cc 行为）
  const bool fs_now = engine_.GetOption(s, "full_shape");
  ::ime::LogF("[handler] SyncUi sid=%u cands=%zu haspos=%d shown=%u full_shape=%d\n",
              session_id, texts.size(), session_has_pos_[session_id] ? 1 : 0,
              shown_session_, fs_now ? 1 : 0);
  win_.SetPreedit(engine_.GetCompositionText(s));
  win_.SetSchemaTitle(engine_.GetCurrentSchema(s));
  if (!texts.empty()) {
    // 跨窗口串台修复（真机 2026-10-13）：只用本会话自己的光标锚点显示。
    // 本会话尚未收到位置 → 隐藏挂起（pending_show_），等 UpdateInputPosition
    // 到位后再补显示；绝不沿用其他会话（其他应用）的旧坐标——否则 WPS→豆包
    // 切换时候选窗先出现在 WPS 的旧位置、再跳回豆包（闪现/乱跳）。
    if (session_has_pos_[session_id]) {
      const RECT& rc = session_pos_[session_id];
      win_.SetPosition(rc.left, rc.bottom + 6 - win_.GetRoleOffsetY(), rc.top);
      win_.Show(texts, hl);
      pending_show_[session_id] = false;
      shown_session_ = session_id;
    } else {
      win_.Hide();
      pending_show_[session_id] = true;
    }
  } else {
    win_.Hide();
    pending_show_[session_id] = false;
  }
}

void BeautypeHandler::SendResponse(DWORD session_id, EatLine eat) {
  const auto s = ToSession(session_id);
  if (s == 0 || !eat)
    return;

  std::wstring body;
  std::vector<std::wstring> actions;

  // 上屏文本（按键/选词产生的 commit 缓冲）
  std::string commit = engine_.GetCommit(s);
  if (::ime::g_log != nullptr) {
    // full_shape 状态与 commit 对读：librime ShapeFormatter（shape.cc Format）
    // 在 full_shape 开启时把 commit 的 ASCII 转全角（如 log→ｌｏｇ），
    // 无候选回车上屏会"变宽"；此日志让全角来源一目了然。
    const bool fs_commit = engine_.GetOption(s, "full_shape");
    ::ime::LogF("[handler] SendResponse GetCommit=%s full_shape=%d\n",
                commit.empty() ? "(空)" : commit.c_str(), fs_commit ? 1 : 0);
  }
  if (!commit.empty()) {
    actions.push_back(L"commit");
    body += L"commit=" + escape_string(Utf8ToWide(commit)) + L"\n";
  }

  // 上下文（组合区 preedit + 候选）——对齐官方 RimeWithWeasel.cpp:797：
  // 仅 is_composing 时发 ctx（官方以 RimeStatus.is_composing 为准，不能只用
  // preedit/candidates 非空判断：commit 后 preedit 已空但组合可能仍在收尾）
  const bool is_composing = engine_.IsComposing(s);
  const std::string preedit_utf8 = engine_.GetCompositionText(s);
  const auto cands = engine_.GetCandidates(s);
  if (is_composing) {
    actions.push_back(L"ctx");
    const std::wstring wpre = Utf8ToWide(preedit_utf8);
    // 官方格式（RimeWithWeasel.cpp:828-838 COMPOSITION 分支）：
    // ctx.preedit=<纯文本（escape 转义后）>，不是 JSON 数组。
    // 官方把 ["...",[]] 留给 UI 层解析；ResponseParser 的 ContextUpdater
    // 直接 unescape 成 preedit.str，写 JSON 会让候选窗显示原始数组。
    body += L"ctx.preedit=" + escape_string(wpre) + L"\n";
    // 官方 ctx.preedit.cursor=start,end,cursor（utf8 字符数；rime 组合默认
    // 无选中 → sel_start=sel_end=0，光标在末尾）。
    // 【修复 2026-10-22 方向键光标】原实现写死 0,0,末尾——librime 按
    // 左/右移动 preedit 光标后光标位置不随响应下发，TSF 宿主光标永远停在
    // 组合末尾（真机"左右方向键不能移动拼音光标"根因）。对齐官方
    // RimeWithWeasel.cpp:831-838：读 librime 真实 sel_start/sel_end/
    // cursor_pos（utf8 字节偏移），经 utf8towcslen 转 wchar 前缀长度下发；
    // start<=end 才发（官方条件），否则客户端保持默认。
    int sel_start = 0, sel_end = 0, cursor_pos = 0;
    if (engine_.GetPreeditCursor(s, &sel_start, &sel_end, &cursor_pos) &&
        sel_start <= sel_end) {
      const int wc_start = utf8towcslen(preedit_utf8.c_str(), sel_start);
      const int wc_end = utf8towcslen(preedit_utf8.c_str(), sel_end);
      const int wc_cursor = utf8towcslen(preedit_utf8.c_str(), cursor_pos);
      // 2026-10-22 方向键光标观测闭环：记录 librime 原始光标（utf8 字节
      // 偏移）与下发的 wchar 前缀长度——确认方向键是否真的让引擎光标移动
      //（若 start/end/cursor 恒不变 → librime navigator 未生效，属配置/
      // 引擎层问题而非下发链路）。
      ::ime::LogF("[handler] preedit.cursor start=%d end=%d cursor=%d (utf8) -> %d,%d,%d (wchar)\n",
                  sel_start, sel_end, cursor_pos, wc_start, wc_end, wc_cursor);
      body += L"ctx.preedit.cursor=" + std::to_wstring(wc_start) + L"," +
              std::to_wstring(wc_end) + L"," + std::to_wstring(wc_cursor) +
              L"\n";
    } else {
      ::ime::LogF("[handler] preedit.cursor 未下发（GetPreeditCursor 失败或 start>end: %d/%d）\n",
                  sel_start, sel_end);
    }
    if (!cands.empty()) {
      // 官方格式：ctx.cand=<boost::archive::text_woarchive 归档的 CandidateInfo>
      weasel::CandidateInfo ci;
      ci.currentPage = 0;
      ci.totalPages = 1;
      ci.highlighted = engine_.GetHighlightedIndex(s);
      ci.is_last_page = true;
      for (const auto& c : cands) {
        ci.candies.emplace_back(Utf8ToWide(c.text));
        ci.comments.emplace_back(Utf8ToWide(c.comment));
      }
      for (std::size_t i = 0; i < cands.size(); ++i)
        ci.labels.emplace_back(std::to_wstring(i + 1));
      std::wstringstream ss;
      boost::archive::text_woarchive oa(ss);
      oa << ci;
      body += L"ctx.cand=" + ss.str() + L"\n";
    }
  }

  // 状态（ascii_mode / composing / schema）——composing 对齐官方
  // RimeWithWeasel.cpp:761：用 RimeStatus.is_composing，不用 preedit 非空推导
  actions.push_back(L"status");
  const bool ascii = engine_.GetOption(s, "ascii_mode");
  const bool composing = engine_.IsComposing(s);
  body += L"status.ascii_mode=" + std::to_wstring(ascii ? 1 : 0) + L"\n";
  body += L"status.composing=" + std::to_wstring(composing ? 1 : 0) + L"\n";
  body += L"status.disabled=0\n";
  // 全角状态读引擎真实值（librime ShapeFormatter 在 full_shape 开启时把
  // commit 的 ASCII 转全角 → 无候选回车上屏拼音变"全角变宽"；此处硬编码 0
  // 会掩盖真实状态，语言栏/设置窗显示失真。librime shape.cc Format 官方行为）
  const bool full_shape = engine_.GetOption(s, "full_shape");
  body += L"status.full_shape=" + std::to_wstring(full_shape ? 1 : 0) + L"\n";
  body += L"status.schema_id=" + Utf8ToWide(engine_.GetCurrentSchema(s)) + L"\n";

  // 配置（内联组合：不启用，候选窗在服务端自绘）
  actions.push_back(L"config");
  body += L"config.inline_preedit=0\n";

  // 头部（action 列表）+ 响应结尾（官方 . 行）
  std::wstring header = L"action=";
  for (std::size_t i = 0; i < actions.size(); ++i) {
    if (i > 0)
      header += L",";
    header += actions[i];
  }
  header += L"\n";

  std::wstring line = header + body + L".\n";
  // 编码诊断（2026-10-07）：dump 发送字节前 40 字节——中文 commit 应为
  // UTF-16LE（如"是"=AF 66），若显示 UTF-8 字节（E6 98 AF）说明发送链路
  // 编码错位（Word 等应用中文乱码根因）。
  if (::ime::g_log != nullptr) {
    const unsigned char* lb =
        reinterpret_cast<const unsigned char*>(line.c_str());
    const std::size_t dump_n =
        (line.size() * sizeof(wchar_t) < 40) ? line.size() * sizeof(wchar_t)
                                             : 40;
    std::string hex;
    char buf[8];
    for (std::size_t i = 0; i < dump_n; ++i) {
      std::snprintf(buf, sizeof(buf), "%02X ", lb[i]);
      hex += buf;
    }
    ::ime::LogF("[handler] SendResponse line字节(dump40)=%s\n", hex.c_str());
  }
  eat(line);
}

// 对齐官方 RimeWithWeasel.cpp:264-292 的 expand_ibus_modifier：把 DLL 传来的
// ibus 位序 mask 展开成 librime RimeModifier 位序——(m & 0xff) 低 8 位原样
//（SHIFT/LOCK/CONTROL/ALT 与 librime 一致）；(m & 0xff00)<<16 把高 8 位左移
// 16 位：RELEASE_MASK(1<<14) → 0x40000000(1<<30, librime kReleaseMask) 等。
// 无此展开时 keyup 的 release 位到不了 librime → Shift 短按切换（1.17.0
// ascii_composer 依赖 release 事件）失效（真机 2026-10-07）。
static int expand_ibus_modifier(int m) {
  return (m & 0xff) | ((m & 0xff00) << 16);
}

// ---- 桌宠按键事件：X11 keysym（引擎键码）→ Windows VK（桌宠 HUD 显示） ----
// 反查 text_service.cpp VkToRimeKey 的正向映射；未覆盖键返回 0（桌宠忽略）。
// 按键 HUD 只需"键位标签"，标点/字母/数字/功能键全覆盖即够；修饰键本身也
// 转发（桌宠卡片显示 Shift/Ctrl/Alt）。
static std::uint32_t XkToVk(int keycode) {
  // 字母 a-z(0x61-0x7A) → VK_A-Z
  if (keycode >= 0x61 && keycode <= 0x7A) {
    return static_cast<std::uint32_t>(keycode - 0x20);
  }
  // 数字/标点半角（keysym 与 VK 同值区）
  if (keycode >= 0x30 && keycode <= 0x39) {
    return static_cast<std::uint32_t>(keycode);
  }
  if (keycode >= 0xFFBE && keycode <= 0xFFC9) {  // F1-F12
    return static_cast<std::uint32_t>(0x70 + (keycode - 0xFFBE));
  }
  switch (keycode) {
    case 0x0020: return 0x20;   // space
    case 0xFF0D: return 0x0D;   // Return
    case 0xFF08: return 0x08;   // BackSpace
    case 0xFF09: return 0x09;   // Tab
    case 0xFF1B: return 0x1B;   // Escape
    case 0xFFE5: return 0x14;   // Caps_Lock
    case 0xFFE1:                 // Shift_L
    case 0xFFE2: return 0x10;   // Shift_R
    case 0xFFE3:                 // Control_L
    case 0xFFE4: return 0x11;   // Control_R
    case 0xFFE9:                 // Alt_L
    case 0xFFEA: return 0x12;   // Alt_R
    case 0xFFE7: return 0x5B;   // Meta_L → VK_LWIN
    case 0xFFE8: return 0x5C;   // Meta_R → VK_RWIN
    case 0xFF55: return 0x21;   // Page_Up
    case 0xFF56: return 0x22;   // Page_Down
    case 0xFF57: return 0x23;   // End
    case 0xFF50: return 0x24;   // Home
    case 0xFF51: return 0x25;   // Left
    case 0xFF52: return 0x26;   // Up
    case 0xFF53: return 0x27;   // Right
    case 0xFF54: return 0x28;   // Down
    case 0xFF63: return 0x2D;   // Insert
    case 0xFFFF: return 0x2E;   // Delete
    // 标点（半角/上档键位映射到同一 VK，HUD 只显示键位）
    case 0x003B: case 0x003A: return 0xBA;   // ; / :        → VK_OEM_1
    case 0x003D: case 0x002B: return 0xBB;   // = / +        → VK_OEM_PLUS
    case 0x002C: case 0x003C: return 0xBC;   // , / <        → VK_OEM_COMMA
    case 0x002D: case 0x005F: return 0xBD;   // - / _        → VK_OEM_MINUS
    case 0x002E: case 0x003E: return 0xBE;   // . / >        → VK_OEM_PERIOD
    case 0x002F: case 0x003F: return 0xBF;   // / / ?        → VK_OEM_2
    case 0x0060: case 0x007E: return 0xC0;   // ` / ~        → VK_OEM_3
    case 0x005B: case 0x007B: return 0xDB;   // [ / {        → VK_OEM_4
    case 0x005C: case 0x007C: return 0xDC;   // \ / |        → VK_OEM_5
    case 0x005D: case 0x007D: return 0xDD;   // ] / }        → VK_OEM_6
    case 0x0027: case 0x0022: return 0xDE;   // ' / "        → VK_OEM_7
    default: return 0;
  }
}

BOOL BeautypeHandler::ProcessKeyEvent(weasel::KeyEvent keyEvent,
                                      DWORD session_id, EatLine eat) {
  // 桌宠按键事件推送（M3-4）：把真实按键转发给桌宠（istyping 打字动画 +
  // 按键 HUD）。best-effort：桌宠不在线/无连接时静默跳过，不影响输入法主功能。
  if (pet_push_ != nullptr) {
    const std::uint32_t vk = XkToVk(static_cast<int>(keyEvent.keycode));
    // 2026-10-17：跳过"点击候选"的模拟键 VK_SELECT(0x29 且无 Shift 修饰)——
    // 该 0x29 是服务端 SendInput 的内部回传键，不是真实按键；转发会让桌宠
    // 按键 HUD 冒出 "key41"（用户实测）。Shift+0（mask 含 Shift）是真实按键
    // 照常转发。
    const bool sim_select = keyEvent.keycode == VK_SELECT &&
                            (keyEvent.mask & kKeyMaskShift) == 0;
    if (vk != 0 && !sim_select) {
      const bool make =
          (keyEvent.mask & ime::kKeyMaskRelease) == 0;  // RELEASE 位=抬起
      pet_push_->SendKeyEvent(vk, 0, make);
    }
  }
  // 维护模式（部署器运行时）：暂停输入，键放行（官方 EndMaintenance 前
  // 引擎不处理任何按键）
  if (maintenance_) {
    return FALSE;
  }
  const auto s = ToSession(session_id);
  if (s == 0)
    return FALSE;
  if (::ime::g_log != nullptr) {
    ::ime::LogF("[handler] ProcessKeyEvent keycode=0x%02X mask=0x%02X\n",
                static_cast<unsigned int>(keyEvent.keycode),
                static_cast<unsigned int>(keyEvent.mask));
  }

  // 点选候选后的模拟键（官方 TSF 点选后 SendInput VK_SELECT 触发响应回传）：
  // 直接消费 select 产生的 commit + 刷新候选窗，不交给 librime 再处理。
  if (keyEvent.keycode == VK_SELECT && select_pending_) {
    select_pending_ = false;
    SyncUi(session_id);
    SendResponse(session_id, eat);
    return TRUE;
  }
  // 【根因 2026-10-17】点击候选 SendInput 的 VK_SELECT(0x29)==ASCII ')' 数值
  // 碰撞：实测 select_pending_ 常为 false（时序错位），0x29 落进 engine_.ProcessKey
  // 被 librime 当 ')' 标点 → punctuator 输出全角"）"追加进候选 commit 多上屏
  // （rime_server.log 799010 GetCommit=打发打发）实测，27 条"）"全部伴随
  // ProcessKey(0x29)）。只拦"无修饰、非 KeyUp"的 0x29（=点击候选模拟键）；
  // Shift+0 打"）"（mask 含 kKeyMaskShift=0x01）与 KeyUp（mask=0x4000，
  // librime 需 release 配对）一律放行，绝不影响正常输入。
  if (keyEvent.keycode == VK_SELECT &&
      (keyEvent.mask & (kKeyMaskRelease | kKeyMaskShift)) == 0) {
    SyncUi(session_id);
    SendResponse(session_id, eat);
    return TRUE;
  }

  const bool eaten =
      engine_.ProcessKey(s, keyEvent.keycode, expand_ibus_modifier(keyEvent.mask));
  if (::ime::g_log != nullptr) {
    ::ime::LogF("[handler] ProcessKey(keycode=0x%02X mask=0x%02X) eaten=%d\n",
                static_cast<unsigned int>(keyEvent.keycode),
                static_cast<unsigned int>(keyEvent.mask), eaten ? 1 : 0);
  }
  // 对齐官方 RimeWithWeasel.cpp:264-292：无论 eaten 与否都 _Respond + _UpdateUI。
  // 修复前 eaten=0（退格/方向键/切换键等）直接 return FALSE 不发响应 → DLL 读
  // 不到服务端真实状态（preedit 清空/组合结束）→ DLL 组合缓存 stale → 退格
  // 不删反而重复上屏、拼音残留（真机根因 2026-10-07）。
  SyncUi(session_id);
  SendResponse(session_id, eat);
  return eaten ? TRUE : FALSE;
}

void BeautypeHandler::CommitComposition(DWORD session_id) {
  // 官方语义：把正在编辑的文字上屏。引擎没有"强制提交"API 时，
  // 用响应把当前组合文本回给 DLL，由 DLL 端上屏 composition。
  SyncUi(session_id);
}

void BeautypeHandler::ClearComposition(DWORD session_id) {
  const auto s = ToSession(session_id);
  if (s == 0)
    return;
  engine_.ClearComposition(s);
  win_.Hide();
}

void BeautypeHandler::SelectCandidateOnCurrentPage(size_t index,
                                                   DWORD session_id) {
  const auto s = ToSession(session_id);
  if (::ime::g_log != nullptr) {
    ::ime::LogF("[handler] SelectCandidateOnCurrentPage index=%zu sid=%lu s=%llu\n",
                index, static_cast<unsigned long>(session_id),
                static_cast<unsigned long long>(s));
  }
  if (s == 0)
    return;
  engine_.SelectCandidate(s, index);
  // 【根因 2026-10-17】librime 1.17.0 的 select_candidate 只移动高亮、不产生
  // commit（实测 GetCommit=(空)，点击候选不上屏）。修复前"上屏"靠 SendInput
  // 的 VK_SELECT(0x29) 落 librime 被当 ')' 标点触发"提交组合+标点" → 上屏
  // "词+）"（既多"）"又依赖误处理）。官方正确做法：选中后用 librime 官方
  // commit_composition 提交组合 → commit=候选词（干净）；VK_SELECT 只负责让
  // TSF 回传响应上屏，不再依赖 librime 对 0x29 的 ')' 误处理。
  // 注：commit 内容不打日志（GetCommit 可能消费缓冲），看 SendResponse 的
  // GetCommit= 行即可。
  engine_.CommitComposition(s);
  select_pending_ = true;
  SyncUi(session_id);
}

bool BeautypeHandler::HighlightCandidateOnCurrentPage(size_t index,
                                                      DWORD session_id,
                                                      EatLine eat) {
  const auto s = ToSession(session_id);
  if (s == 0)
    return false;
  // 引擎不暴露"设高亮"API：用上下键语义模拟（librime Up/Down 切换高亮）。
  const int cur = engine_.GetHighlightedIndex(s);
  const auto cands = engine_.GetCandidates(s);
  const int want = static_cast<int>(index);
  if (want == cur)
    return true;
  int delta = want > cur ? 1 : -1;
  int guard = 0;
  while (guard++ < static_cast<int>(cands.size()) + 2) {
    engine_.ProcessKey(s, want > cur ? VK_DOWN : VK_UP, 0);
    if (engine_.GetHighlightedIndex(s) == want)
      break;
  }
  SyncUi(session_id);
  SendResponse(session_id, eat);
  return true;
}

bool BeautypeHandler::ChangePage(bool backward, DWORD session_id, EatLine eat) {
  const auto s = ToSession(session_id);
  if (s == 0)
    return false;
  engine_.ProcessKey(s, backward ? VK_PRIOR : VK_NEXT, 0);
  SyncUi(session_id);
  SendResponse(session_id, eat);
  return true;
}

void BeautypeHandler::FocusIn(DWORD param, DWORD session_id) {
  if (::ime::g_log != nullptr) {
    ::ime::LogF("[handler] FocusIn sid=%lu last=%lu\n",
                static_cast<unsigned long>(session_id),
                static_cast<unsigned long>(last_active_session_));
  }
  last_active_session_ = session_id;
  if (sessions_.count(session_id) == 0)
    return;
  // 焦点切到新会话：收起可能还残留的其他会话候选窗（防「输入框还在上一个
  // 应用里」）。本会话自己的候选窗等位置上报后经 SyncUi 正常显示。
  if (shown_session_ != 0 && shown_session_ != session_id) {
    win_.Hide();
    shown_session_ = 0;
  }
}

void BeautypeHandler::FocusOut(DWORD param, DWORD session_id) {
  // 失焦：隐藏候选窗（官方行为）；组合不清空（回到焦点继续）。
  win_.Hide();
  shown_session_ = 0;
  pending_show_[session_id] = false;
}

void BeautypeHandler::ImeActivated(bool activated, DWORD session_id) {
  // M4+：首次切换到 Beautype 输入法 → 拉起桌宠（开机不预启动；切走不隐藏，
  // 只是 Beautype 不再产生按键事件，桌宠动画自然回到待机）。
  // 用户右键退出过桌宠（bye）→ 本次服务生命周期不再自动拉起（重启服务恢复）。
  if (activated && !pet_user_exited_ && pet_launcher_) {
    pet_launcher_();
  }
  ::ime::LogF("[handler] ImeActivated activated=%d sid=%u exited=%d\n",
              activated ? 1 : 0, session_id, pet_user_exited_ ? 1 : 0);
}

void BeautypeHandler::UpdateInputPosition(RECT const& rc, DWORD session_id) {
  // 按会话缓存各自的光标锚点（跨窗口串台修复的数据基础）
  session_pos_[session_id] = rc;
  session_has_pos_[session_id] = true;
  // 诊断日志（真机 2026-10-13：位置路径此前无日志，无法确认服务端实际收到
  // 哪些会话的哪些坐标——闪动/串台定位全靠猜。此行带会话 id + 收到坐标 +
  // 当前可见归属 + 挂起状态，一次按键一行，跨会话串台一眼可见）
  ::ime::LogF("[handler] UpdateInputPosition sid=%u rc=(%d,%d)-(%d,%d) shown=%u pending=%d\n",
              session_id, rc.left, rc.top, rc.right, rc.bottom, shown_session_,
              pending_show_[session_id] ? 1 : 0);
  // 候选窗锚定到光标下方：候选框（在窗口底部）紧贴组合文本下方 6px。
  // 月亮比候选框高出来的部分（offset）往上溢出，不把候选框推远。
  // 用户改月亮大小时 offset 自动变化，候选框位置始终紧贴光标。
  const int offset = win_.GetRoleOffsetY();
  // 仅当候选窗当前可见且属于本会话（或已隐藏）时才直接移动窗口：
  // 防止其他会话的位置上报把正在显示的候选窗拖走（跨窗口串台）。
  if (shown_session_ == 0 || shown_session_ == session_id) {
    win_.SetPosition(rc.left, rc.bottom + 6 - offset, rc.top);
  }
  // 本会话有挂起的候选内容（位置未到时 SyncUi 已隐藏）→ 现在补显示
  if (pending_show_[session_id]) {
    pending_show_[session_id] = false;
    SyncUi(session_id);
  }
}

void BeautypeHandler::SetOption(DWORD session_id, const std::string& opt,
                                bool val) {
  const auto s = ToSession(session_id);
  if (s == 0)
    return;
  engine_.SetOption(s, opt, val);
}

void BeautypeHandler::SetAsciiModeForAll(bool ascii) {
  for (auto& [sid, rime_sid] : sessions_)
    engine_.SetOption(rime_sid, "ascii_mode", ascii);
}

void BeautypeHandler::SelectSchemaForAll(const std::string& schema_id) {
  for (auto& [sid, rime_sid] : sessions_)
    engine_.SelectSchema(rime_sid, schema_id);
}

void BeautypeHandler::StartMaintenance() {
  // 维护模式开始（部署器运行）：暂停输入 + 隐藏候选窗。
  // 官方 RimeWithWeasel.cpp:475 同语义（部署线程独占 user 数据前先停引擎）。
  maintenance_ = true;
  win_.Hide();
  if (maintenance_listener_) maintenance_listener_(true);  // 维护期托盘图标（官方 disabled→ShowIcon）
  if (::ime::g_log != nullptr) {
    ::ime::LogF( "[handler] 维护模式开始（部署器运行，输入暂停）\n");
  }
}

void BeautypeHandler::EndMaintenance() {
  // 维护模式结束：恢复输入（引擎在部署期间未被销毁，直接恢复处理按键）
  maintenance_ = false;
  if (maintenance_listener_) maintenance_listener_(false);  // 收回托盘图标
  if (::ime::g_log != nullptr) {
    ::ime::LogF( "[handler] 维护模式结束（输入恢复）\n");
  }
}

void BeautypeHandler::UpdateColorTheme(BOOL darkMode) {
  // 深色模式跟随（官方 UpdateColorTheme 虚接口：TSF 检测系统深色 → 服务端换肤）。
  // 实际换肤由 server_main 监听 WM_SETTINGCHANGE 完成（避免 handler 持有主题状态），
  // 此处仅记录并隐藏候选窗（换肤后下次 SyncUi 以新主题重建）。
  if (::ime::g_log != nullptr) {
    ::ime::LogF( "[handler] 深色模式变化：%s\n",
                 darkMode ? "dark" : "light");
  }
  win_.Hide();
}

}  // namespace ime::server
