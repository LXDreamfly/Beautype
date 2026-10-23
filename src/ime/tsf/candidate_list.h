// UWP 集成候选（对齐小狼毫 WeaselTSF/CandidateList.cpp 的 CCandidateList）。
//
// 背景：自绘候选窗是顶层 Win32 窗口，UWP/XAML 应用（如 Windows 搜索框、
// 商店应用）所在线程无法被普通顶层窗口覆盖，必须走 TSF 的 UI element 机制
// 才能显示候选。本类实现 ITfCandidateListUIElementBehavior（含
// ITfUIElement / ITfCandidateListUIElement）+ ITfIntegratableCandidateListUIElement
// （Win10 集成候选样式），经 ITfUIElementMgr::BeginUIElement/UpdateUIElement 注册，
// TSF 会把候选数据交给应用的候选 UI 渲染。
//
// 与自绘候选窗并行工作：Win32 应用用自绘窗（text_service 原有路径），
// UWP 应用用本 UI element。text_service 每次刷新候选时同步调用 UpdateData。
#pragma once

#include <windows.h>
#include <msctf.h>
#include <ctffunc.h>  // ITfIntegratableCandidateListUIElement
#include <functional>
#include <string>
#include <vector>

namespace ime {
namespace tsf {

// 微软候选 UI element 集成类
class CandidateListUI final : public ITfCandidateListUIElementBehavior,
                              public ITfIntegratableCandidateListUIElement {
 public:
  explicit CandidateListUI(ITfThreadMgr* thread_mgr);
  virtual ~CandidateListUI();

  // text_service 候选刷新时调用：候选文本、高亮序号、是否组合中。
  // composing=false 或 texts 为空 → 结束 UI element（隐藏）。
  void UpdateData(const std::vector<std::wstring>& texts, UINT highlighted,
                  bool composing);

  // 组合中止回调（UWP 用户按 Esc/点击 Abort 时调用，由 text_service 设置）
  void SetAbortCallback(std::function<void()> cb) { abort_cb_ = std::move(cb); }

  // Deactivate 时调用：结束 UI element 并清空数据
  void DestroyAll();

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  STDMETHODIMP_(ULONG) AddRef() override;
  STDMETHODIMP_(ULONG) Release() override;

  // ITfUIElement
  STDMETHODIMP GetDescription(BSTR* pbstr) override;
  STDMETHODIMP GetGUID(GUID* pguid) override;
  STDMETHODIMP Show(BOOL bShow) override;
  STDMETHODIMP IsShown(BOOL* pbShow) override;

  // ITfCandidateListUIElement
  STDMETHODIMP GetUpdatedFlags(DWORD* pdwFlags) override;
  STDMETHODIMP GetDocumentMgr(ITfDocumentMgr** ppdim) override;
  STDMETHODIMP GetCount(UINT* puCount) override;
  STDMETHODIMP GetSelection(UINT* puIndex) override;
  STDMETHODIMP GetString(UINT uIndex, BSTR* pstr) override;
  STDMETHODIMP GetPageIndex(UINT* pIndex, UINT uSize, UINT* puPageCnt) override;
  STDMETHODIMP SetPageIndex(UINT* pIndex, UINT uPageCnt) override;
  STDMETHODIMP GetCurrentPage(UINT* puPage) override;

  // ITfCandidateListUIElementBehavior
  STDMETHODIMP SetSelection(UINT nIndex) override;
  STDMETHODIMP Finalize() override;
  STDMETHODIMP Abort() override;

  // ITfIntegratableCandidateListUIElement（Win10 集成样式）
  STDMETHODIMP SetIntegrationStyle(GUID guidIntegrationStyle) override;
  STDMETHODIMP GetSelectionStyle(
      TfIntegratableCandidateListSelectionStyle* ptfSelectionStyle) override;
  STDMETHODIMP OnKeyDown(WPARAM wParam, LPARAM lParam, BOOL* pfEaten) override;
  STDMETHODIMP ShowCandidateNumbers(BOOL* pfShow) override;
  STDMETHODIMP FinalizeExactCompositionString() override;

 private:
  void StartUI();
  void EndUI();
  void UpdateUIElement();

  volatile ULONG ref_;
  ITfThreadMgr* thread_mgr_;  // 借用指针（text_service 生命周期内有效，不 AddRef）
  std::vector<std::wstring> texts_;
  UINT highlighted_;
  bool composing_;
  bool shown_;
  bool ui_started_;
  DWORD ui_element_id_;
  std::function<void()> abort_cb_;
};

}  // namespace tsf
}  // namespace ime
