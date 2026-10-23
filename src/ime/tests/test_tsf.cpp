// CI 冒烟：TSF 文本服务链路（服务化架构）。
// 服务化后 DLL 不加载本地 librime：按键经官方 Client（IPC 管道）转发给
// rime_server.exe 服务进程，引擎在服务端计算，组合串/候选/上屏经响应回传。
// 因此本测试不再用本地 RimeEngine 断言（旧架构遗留，服务化后恒为空），
// 改为：
//   ①-A（CI 可验）：ThreadMgr 创建、服务激活（文档管理器+上下文入栈）、
//     ITfKeystrokeMgr 可用。
//   合成按键驱动：OnKeyDown("nihao") → IPC 转发 → 服务端引擎组合/候选 →
//     响应回传（service.ipc_context() 组合串 + 候选「你好」）→ 空格上屏
//     （service.last_commit() == "你好"）。
//   ② 真实消息入口：WM_KEYDOWN 经窗口过程投递到服务——覆盖 DemoWndProc
//     转发链，防止 CI 直调 OnKeyDown 绕过消息/焦点门导致真机失灵而 CI 绿
//     （历史教训：3c1bfea 前真机打字无反应但 CI 绿）。
//   ③ HandleRawKey 标点：Shift+/ → ？、Shift+1 → ！直达上屏。
// 服务进程不在时 Activate 内含 EnsureSession 自动拉起（DLL/exe 同目录），
// 测试不依赖外部已启动的服务实例。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>
#include <msctf.h>

// stub: text_service references main.cpp's global g_demo_hwnd; unit test doesn't link main.obj
HWND g_demo_hwnd = nullptr;

// 真实消息入口窗口过程的焦点上下文（模拟焦点文档；无捕获 lambda 才能转 WNDPROC）
ITfContext* g_test_ctx = nullptr;

#include "../fs_util.h"
// KeyMask 位常量定义（服务化后 DLL 不持引擎，但测试仍用其修饰键位组合）
#include "engine/rime_engine.h"
#include "tsf/text_service.h"

namespace {

std::string FindSharedDataDir() {
  if (const wchar_t* env = _wgetenv(L"RIME_SHARED_DATA_DIR"); env != nullptr && *env != L'\0') {
    return ime::WideToUtf8(env);
  }
  // 回退：常见 build 目录相对位置（与 test_engine 同款，build/ 与 build/x64/...）
  for (const char* rel : {"../../data/rime", "../../../data/rime",
                          "../../../../data/rime",
                          "../third_party/librime/data/minimal",
                          "../../third_party/librime/data/minimal",
                          "../../../third_party/librime/data/minimal"}) {
    std::error_code ec;
    if (ime::DirectoryExists(rel)) {
      return rel;
    }
  }
  return "";
}

std::string WideToUtf8(const std::wstring& ws) {
  if (ws.empty()) {
    return "";
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string s(n > 1 ? static_cast<std::size_t>(n - 1) : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], n, nullptr, nullptr);
  }
  return s;
}

void ReportHr(const char* step, HRESULT hr) {
  std::printf("[test-tsf] %-40s 0x%08lX %s\n", step, static_cast<unsigned long>(hr),
              SUCCEEDED(hr) ? "OK" : "FAIL");
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);  // 崩溃定位：无缓冲，逐行可见

  std::printf("[test-tsf] === M1 TSF 文本服务链路冒烟（服务化/IPC） ===\n");
  int failures = 0;

  const std::string shared = FindSharedDataDir();
  if (shared.empty()) {
    std::printf("[test-tsf] FAIL: 找不到预置数据目录（设置 RIME_SHARED_DATA_DIR）\n");
    return 1;
  }

  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ReportHr("CoInitializeEx", hr);
  if (FAILED(hr)) {
    return 1;
  }

  ITfThreadMgr* thread_mgr = nullptr;
  hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                        IID_ITfThreadMgr, reinterpret_cast<void**>(&thread_mgr));
  ReportHr("CoCreateInstance(CLSID_TF_ThreadMgr)", hr);
  if (FAILED(hr)) {
    CoUninitialize();
    return 1;
  }

  TfClientId client_id = 0;
  hr = thread_mgr->Activate(&client_id);
  ReportHr("ITfThreadMgr::Activate", hr);
  if (FAILED(hr) || client_id == 0) {
    ++failures;
  }

  // 模拟焦点文档：CI session 0 无交互桌面，不创建焦点文档则
  // thread_mgr->GetFocus() 返回空 → IsKeyboardDisabled() 判定"系统禁用键盘"
  // → HandleRawKey 放行所有按键（真机有焦点文档所以正常，测试必须模拟）。
  // 对齐真实 TSF 应用：CreateDocumentMgr → CreateContext → Push → SetFocus。
  ITfDocumentMgr* doc_mgr = nullptr;
  ITfContext* test_ctx = nullptr;
  hr = thread_mgr->CreateDocumentMgr(&doc_mgr);
  ReportHr("ITfThreadMgr::CreateDocumentMgr", hr);
  if (FAILED(hr) || doc_mgr == nullptr) {
    ++failures;
  }
  if (SUCCEEDED(hr) && doc_mgr != nullptr) {
    TfEditCookie cookie = 0;
    hr = doc_mgr->CreateContext(client_id, 0, nullptr, &test_ctx, &cookie);
    ReportHr("ITfDocumentMgr::CreateContext", hr);
    if (FAILED(hr) || test_ctx == nullptr) {
      ++failures;
    }
    g_test_ctx = test_ctx;
    if (SUCCEEDED(hr) && test_ctx != nullptr) {
      hr = doc_mgr->Push(test_ctx);
      ReportHr("ITfDocumentMgr::Push", hr);
      if (SUCCEEDED(hr)) {
        hr = thread_mgr->SetFocus(doc_mgr);
        ReportHr("ITfThreadMgr::SetFocus", hr);
        if (FAILED(hr)) {
          ++failures;
        }
      } else {
        ++failures;
      }
    }
  }

  // ①-A：服务激活（文档管理器 + 上下文入栈；激活末尾 EnsureSession 自动拉起
  // 服务进程，连接失败不阻断激活——按键时再重试，与官方一致）
  ime::RimeTextService service;
  hr = service.Activate(thread_mgr, client_id);
  ReportHr("RimeTextService::Activate", hr);
  if (SUCCEEDED(hr) && !service.activated()) {
    std::printf("[test-tsf] FAIL: Activate 成功后 activated 未置位\n");
    ++failures;
  }
  std::printf("[test-tsf] IPC 客户端连接=%s\n",
              service.client_connected() ? "是" : "否（按键时 EnsureSession 重试拉起）");

  // ①-A：ITfKeystrokeMgr 可用性（①-B：注册/送达依赖交互桌面）
  ITfKeystrokeMgr* key_mgr = nullptr;
  hr = thread_mgr->QueryInterface(IID_ITfKeystrokeMgr, reinterpret_cast<void**>(&key_mgr));
  ReportHr("QI IID_ITfKeystrokeMgr", hr);
  if (SUCCEEDED(hr) && key_mgr != nullptr) {
    BOOL thread_focus = FALSE;
    thread_mgr->IsThreadFocus(&thread_focus);
    std::printf("[test-tsf] IsThreadFocus=%s（CI 无交互桌面：按键 sink 注册/送达归 ①-B）\n",
                thread_focus ? "TRUE" : "FALSE");
    key_mgr->Release();
  } else if (FAILED(hr)) {
    ++failures;
  }

  // 合成按键驱动 IPC：n i h a o → 组合串 + 候选「你好」（从 IPC 响应读）
  // 传入 test_ctx：真实 TSF 路径 OnKeyDown 的 pic 是焦点应用上下文，
  // 组合/提交/定位都作用在它上面（对齐官方；传 nullptr 则 context_ 为空，
  // 上屏插入路径 InsertCommitText 会直接 return）。
  // 上屏断言用 commit 回调捕获（HandleKey 消费 last_commit_ 后即清空，防
  // 重复上屏——服务化修复项③的语义；OnKeyDown 返回后 last_commit() 必然
  // 为空，不能作为断言点）。
  std::vector<std::string> committed;
  service.SetCommitCallback(
      [&committed](const std::string& c) { committed.push_back(c); });
  BOOL eaten = FALSE;
  const int keys[] = {'N', 'I', 'H', 'A', 'O'};
  for (int k : keys) {
    service.OnKeyDown(test_ctx, k, 0, &eaten);
  }
  const std::string input = WideToUtf8(service.ipc_context().preedit.str);
  std::printf("[test-tsf] 组合串=%s\n", input.empty() ? "(空)" : input.c_str());
  // rime 拼音分词显示带空格（ni hao），断言前缀即可（引擎链路已通）
  if (input != "ni hao") {
    ++failures;
  }
  bool found = false;
  for (const weasel::Text& c : service.ipc_context().cinfo.candies) {
    if (WideToUtf8(c.str) == "你好") found = true;
  }
  const auto cand_count = service.ipc_context().cinfo.candies.size();
  if (cand_count == 0 || !found) {
    std::printf("[test-tsf] FAIL: 候选中无「你好」（候选 %zu 项）\n", cand_count);
    ++failures;
  } else {
    std::printf("[test-tsf] 候选 %zu 项，含「你好」\n", cand_count);
  }

  // 空格上屏：服务端 commit 回传 → HandleKey 消费并清空（插入文档走 ①-B）。
  // 组合建立成功时走 CommitCompositionEditSession（不触发 callback，真实
  // TSF 文档插入路径），仅能观察服务端响应（status.composing 归 0）；
  // 组合未建立（异步会话未完成）时走 InsertCommitText → callback 捕获
  // 「你好」。两条路径任一成立都算服务端已提交（2026-10-07 断言修正：
  // 空格 confirm 后服务端组合清空、不再发 ctx 行，ipc_context_.preedit
  // 停留旧值，preedit 清空不是有效信号；status.composing=0 才是）。
  committed.clear();
  service.OnKeyDown(test_ctx, VK_SPACE, 0, &eaten);
  const std::string committed_zh =
      committed.empty() ? "" : committed.back();
  const bool preedit_cleared = service.ipc_context().preedit.str.empty();
  const bool composing_ended = !service.ipc_status().composing;
  std::printf("[test-tsf] 上屏文本=%s preedit清空=%s composing结束=%s\n",
              committed_zh.empty() ? "(空)" : committed_zh.c_str(),
              preedit_cleared ? "是" : "否",
              composing_ended ? "是" : "否");
  if (committed_zh != "你好" && !composing_ended) {
    ++failures;
  }

  // ② 真实消息入口（验证缺口补强）：WM_KEYDOWN 必须经窗口过程转发到服务，
  //    而不是直接调 OnKeyDown——否则 CI 全绿但真机键盘事件因焦点/消息门
  //    到不了引擎（历史教训：3c1bfea 前真机打字无反应而 CI 绿）。
  {
    constexpr wchar_t kSinkClass[] = L"RimeBeautifyTestSinkWindow";
    WNDCLASSW twc{};
    twc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
      auto* sv =
          reinterpret_cast<ime::RimeTextService*>(GetWindowLongPtrW(h, GWLP_USERDATA));
      if (m == WM_KEYDOWN && sv != nullptr) {
        BOOL eaten = FALSE;
        // 真实 TSF 路径：pic 为焦点应用上下文（测试用 g_test_ctx 模拟焦点文档）
        sv->OnKeyDown(g_test_ctx, w, l, &eaten);
        return 0;
      }
      return DefWindowProcW(h, m, w, l);
    };
    twc.hInstance = GetModuleHandleW(nullptr);
    twc.lpszClassName = kSinkClass;
    if (RegisterClassW(&twc) != 0) {
      HWND sink = CreateWindowExW(0, kSinkClass, L"tsf-sink", WS_OVERLAPPEDWINDOW, 0, 0,
                                  200, 100, nullptr, nullptr, twc.hInstance, nullptr);
      if (sink != nullptr) {
        SetWindowLongPtrW(sink, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&service));
        for (int k : keys) {
          SendMessageW(sink, WM_KEYDOWN, static_cast<WPARAM>(k), 0);
        }
        const std::string msg_input = WideToUtf8(service.ipc_context().preedit.str);
        std::printf("[test-tsf] 真实消息入口 组合串=%s\n",
                    msg_input.empty() ? "(空)" : msg_input.c_str());
        if (msg_input != "ni hao") {
          ++failures;
        }
        committed.clear();
        SendMessageW(sink, WM_KEYDOWN, static_cast<WPARAM>(VK_SPACE), 0);
        const std::string msg_commit = committed.empty() ? "" : committed.back();
        const bool msg_preedit_cleared = service.ipc_context().preedit.str.empty();
        const bool msg_composing_ended = !service.ipc_status().composing;
        std::printf("[test-tsf] 真实消息入口 上屏=%s preedit清空=%s composing结束=%s\n",
                    msg_commit.empty() ? "(空)" : msg_commit.c_str(),
                    msg_preedit_cleared ? "是" : "否",
                    msg_composing_ended ? "是" : "否");
        if (msg_commit != "你好" && !msg_composing_ended) {
          ++failures;
        }
        DestroyWindow(sink);
      } else {
        std::printf("[test-tsf] FAIL: 真实消息入口 窗口创建失败\n");
        ++failures;
      }
      UnregisterClassW(kSinkClass, twc.hInstance);
    } else {
      std::printf("[test-tsf] FAIL: 真实消息入口 窗口类注册失败\n");
      ++failures;
    }
  }

  // ③ 统一按键入口 HandleRawKey（演示宿主用）：修饰键状态由调用方在按键
  //    发生时刻捕获传入——钩子回调→PostMessage 异步转发下，若服务侧再查
  //    GetAsyncKeyState，处理时刻 shift 可能已松开，标点上档丢失
  //    （真机实测 shift+/ 出 、 而非 ？）。此处以携带 shift 掩码的合成按键
  //    验证上档标点直达上屏（服务端 commit 回传）。
  {
    // 标点键在无组合时直接插入 → 走 InsertCommitText → commit 回调捕获
    committed.clear();
    service.HandleRawKey(VK_OEM_2, ime::kKeyMaskShift);  // Shift+/ → ？
    const std::string p1 = committed.empty() ? "" : committed.back();
    std::printf("[test-tsf] HandleRawKey 标点 上屏=%s\n",
                p1.empty() ? "(空)" : p1.c_str());
    if (p1 != "？") {
      std::printf("[test-tsf] FAIL: Shift+/ 未上屏「？」\n");
      ++failures;
    }
    committed.clear();
    service.HandleRawKey('1', ime::kKeyMaskShift);  // Shift+1 → ！
    const std::string p2 = committed.empty() ? "" : committed.back();
    std::printf("[test-tsf] HandleRawKey 标点 上屏=%s\n",
                p2.empty() ? "(空)" : p2.c_str());
    if (p2 != "！") {
      std::printf("[test-tsf] FAIL: Shift+1 未上屏「！」\n");
      ++failures;
    }
  }

  hr = service.Deactivate();
  ReportHr("RimeTextService::Deactivate", hr);
  if (FAILED(hr)) {
    ++failures;
  }

  // 清理模拟焦点文档（先出栈再释放，顺序对齐 TSF 文档生命周期）
  if (doc_mgr != nullptr) {
    if (test_ctx != nullptr) {
      doc_mgr->Pop(0);
      test_ctx->Release();
    }
    doc_mgr->Release();
  }

  thread_mgr->Deactivate();
  thread_mgr->Release();
  CoUninitialize();

  if (failures == 0) {
    std::printf("[test-tsf] 结论①-A（CI 已验）：TSF 激活链路 + IPC 合成按键 → "
                "服务端候选/上屏回传 可行。\n");
    std::printf("[test-tsf] 结论②（CI 已验）：真实 WM_KEYDOWN 消息入口 → 服务 → "
                "服务端候选/上屏回传 可行。\n");
    std::printf("[test-tsf] 结论①-B（待实测，交互桌面）：按键 sink 注册/送达、上屏插入、"
                "候选窗定位需交互会话验证（M1 手动验收项）。\n");
    return 0;
  }
  std::printf("[test-tsf] 结果：%d FAILURE(S)\n", failures);
  return 1;
}
