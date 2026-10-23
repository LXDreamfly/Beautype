// IME 服务化 IPC 协议（v1）：DLL（TSF 前端）⇄ 服务进程（引擎+候选窗+托盘）。
// 帧格式复用 shared/ipc/frame.h（4 字节大端长度 + UTF-8 JSON 载荷）。
// 消息分类对齐官方 weasel WeaselIPC 模块：
//   ActionLoader/Committer（DLL→Server：按键/选词/提交类动作）
//   ContextUpdater/Styler（Server→DLL：候选/上下文/状态推送）
//   ResponseParser（Server→DLL：动作应答）
// 载荷为极简 JSON（字段固定，见各结构；不引入第三方 JSON 依赖）。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rime_beautify::ipc {

// 管道名（服务进程创建、DLL 客户端连接；仅本机、受限访问）。
inline constexpr wchar_t kImePipeName[] = L"\\\\.\\pipe\\beautype.ime";

// 消息类型（t 字段）。
enum class ImeMsg : int {
  kHello = 1,          // DLL→Server：连接握手（ver）
  kHelloAck = 2,       // Server→DLL：握手应答（ok）
  kKeyDown = 3,        // DLL→Server：按键（vk, mask）
  kSelectCandidate = 4,// DLL→Server：选择候选（i）
  kSetOption = 5,      // DLL→Server：设置选项（k, v）
  kUpdatePosition = 6, // DLL→Server：候选窗屏幕位置（x, y）——GetTextExt 结果
  kCandidates = 7,     // Server→DLL：候选/上下文快照
  kCommitText = 8,     // Server→DLL：上屏文本
  kStatus = 9,         // Server→DLL：状态推送（ascii_mode / schema）
  kAck = 10,           // Server→DLL：动作应答
  kPing = 11,          // 保活
  kPong = 12,          // 保活应答
};

// DLL→Server 载荷。
struct KeyDownMsg {
  int vk = 0;     // 虚拟键码
  int mask = 0;   // ime::KeyMask 组合（shift/caps/ctrl/alt）
};
struct SelectCandidateMsg {
  int index = 0;
};
struct SetOptionMsg {
  std::string key;
  bool value = false;
};
struct UpdatePositionMsg {
  int x = 0;
  int y = 0;
};
struct HelloMsg {
  int ver = 1;
};

// Server→DLL 载荷。
struct CandidateMsg {
  std::string input;              // 当前拼音/编码
  std::string preedit;            // 组合区文本（已选词+剩余拼音）
  std::vector<std::string> list;  // 候选文本
  int highlighted = -1;           // 高亮下标
  bool composing = false;         // 是否行内组合中
  bool ascii_mode = false;        // 中英状态
  std::string schema;             // 当前方案 id
};
struct CommitMsg {
  std::string text;
};
struct StatusMsg {
  bool ascii_mode = false;
  std::string schema;
};
struct AckMsg {
  bool ok = false;
};

// ---- 极简 JSON 编解码（固定字段；非法输入返回 false）----
std::string ImeMsgToJson(ImeMsg t);
bool ImeMsgFromJson(const std::string& json, ImeMsg* t);

std::string KeyDownToJson(const KeyDownMsg& m);
bool KeyDownFromJson(const std::string& json, KeyDownMsg* m);
std::string SelectCandidateToJson(const SelectCandidateMsg& m);
std::string SetOptionToJson(const SetOptionMsg& m);
bool SetOptionFromJson(const std::string& json, SetOptionMsg* m);
std::string UpdatePositionToJson(const UpdatePositionMsg& m);
bool UpdatePositionFromJson(const std::string& json, UpdatePositionMsg* m);
std::string HelloToJson(const HelloMsg& m);

std::string CandidateToJson(const CandidateMsg& m);
bool CandidateFromJson(const std::string& json, CandidateMsg* m);
std::string CommitToJson(const CommitMsg& m);
bool CommitFromJson(const std::string& json, CommitMsg* m);
std::string StatusToJson(const StatusMsg& m);
bool StatusFromJson(const std::string& json, StatusMsg* m);
std::string AckToJson(const AckMsg& m);
bool AckFromJson(const std::string& json, AckMsg* m);

}  // namespace rime_beautify::ipc
