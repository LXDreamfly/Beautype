// 见 candidate_list.h 顶部注释。实现对齐小狼毫 CCandidateList 的接口语义。
#include "tsf/candidate_list.h"

#include "i18n/strings.h"
#include <objbase.h>

namespace ime {
namespace tsf {

// C4250：diamond 继承（两接口链各自含 IUnknown），本类显式实现一份 IUnknown
// 三方法，警告无实际影响（weasel 同样结构）。WX 下关闭该警告。
#pragma warning(push)
#pragma warning(disable : 4250)

// 本 UI element 的稳定 GUID（weasel 亦自定：36c3c795-...）
static const GUID kCandidateListUIElementGuid = {
    0x36c3c795, 0x7159, 0x45aa, {0xab, 0x12, 0x30, 0x22, 0x9a, 0x51, 0xdb, 0xd3}};

CandidateListUI::CandidateListUI(ITfThreadMgr* thread_mgr)
    : ref_(1),
      thread_mgr_(thread_mgr),
      highlighted_(0),
      composing_(false),
      shown_(false),
      ui_started_(false),
      ui_element_id_(0) {}

CandidateListUI::~CandidateListUI() {
  EndUI();
}

void CandidateListUI::UpdateData(const std::vector<std::wstring>& texts,
                                 UINT highlighted, bool composing) {
  texts_ = texts;
  highlighted_ = highlighted;
  composing_ = composing;
  shown_ = composing_ && !texts_.empty();
  if (shown_) {
    StartUI();
    if (ui_started_) {
      UpdateUIElement();
    }
  } else {
    EndUI();
  }
}

void CandidateListUI::DestroyAll() {
  EndUI();
  texts_.clear();
  highlighted_ = 0;
  composing_ = false;
  shown_ = false;
}

STDMETHODIMP CandidateListUI::QueryInterface(REFIID riid, void** ppv) {
  if (ppv == nullptr) {
    return E_INVALIDARG;
  }
  *ppv = nullptr;
  if (IsEqualIID(riid, IID_IUnknown) ||
      IsEqualIID(riid, IID_ITfUIElement) ||
      IsEqualIID(riid, IID_ITfCandidateListUIElement) ||
      IsEqualIID(riid, IID_ITfCandidateListUIElementBehavior)) {
    *ppv = static_cast<ITfCandidateListUIElementBehavior*>(this);
  } else if (IsEqualIID(riid, IID_ITfIntegratableCandidateListUIElement)) {
    *ppv = static_cast<ITfIntegratableCandidateListUIElement*>(this);
  }
  if (*ppv != nullptr) {
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CandidateListUI::AddRef() { return ++ref_; }

STDMETHODIMP_(ULONG) CandidateListUI::Release() {
  const ULONG n = --ref_;
  if (n == 0) {
    delete this;
  }
  return n;
}

STDMETHODIMP CandidateListUI::GetDescription(BSTR* pbstr) {
  if (pbstr == nullptr) {
    return E_INVALIDARG;
  }
  ime::i18n::ReloadLanguage();
  *pbstr = SysAllocString(
      ime::i18n::TrW(ime::i18n::Str::kCandidateListDescription));
  return S_OK;
}

STDMETHODIMP CandidateListUI::GetGUID(GUID* pguid) {
  if (pguid == nullptr) {
    return E_INVALIDARG;
  }
  *pguid = kCandidateListUIElementGuid;
  return S_OK;
}

STDMETHODIMP CandidateListUI::Show(BOOL bShow) {
  shown_ = (bShow != FALSE);
  return S_OK;
}

STDMETHODIMP CandidateListUI::IsShown(BOOL* pbShow) {
  if (pbShow == nullptr) {
    return E_INVALIDARG;
  }
  *pbShow = shown_ ? TRUE : FALSE;
  return S_OK;
}

STDMETHODIMP CandidateListUI::GetUpdatedFlags(DWORD* pdwFlags) {
  if (pdwFlags == nullptr) {
    return E_INVALIDARG;
  }
  *pdwFlags = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION |
              TF_CLUIE_STRING | TF_CLUIE_CURRENTPAGE;
  return S_OK;
}

STDMETHODIMP CandidateListUI::GetDocumentMgr(ITfDocumentMgr** ppdim) {
  if (ppdim == nullptr) {
    return E_INVALIDARG;
  }
  *ppdim = nullptr;
  if (thread_mgr_ == nullptr) {
    return E_FAIL;
  }
  return thread_mgr_->GetFocus(ppdim);
}

STDMETHODIMP CandidateListUI::GetCount(UINT* puCount) {
  if (puCount == nullptr) {
    return E_INVALIDARG;
  }
  *puCount = static_cast<UINT>(texts_.size());
  return S_OK;
}

STDMETHODIMP CandidateListUI::GetSelection(UINT* puIndex) {
  if (puIndex == nullptr) {
    return E_INVALIDARG;
  }
  *puIndex = highlighted_;
  return S_OK;
}

STDMETHODIMP CandidateListUI::GetString(UINT uIndex, BSTR* pstr) {
  if (pstr == nullptr) {
    return E_INVALIDARG;
  }
  *pstr = nullptr;
  if (uIndex >= texts_.size()) {
    return E_INVALIDARG;
  }
  // 候选文本可能含组合字符；按 weasel 写法把终止符也计入长度
  const std::wstring& s = texts_[uIndex];
  *pstr = SysAllocStringLen(s.c_str(), static_cast<UINT>(s.size()) + 1);
  return (*pstr != nullptr) ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CandidateListUI::GetPageIndex(UINT* pIndex, UINT uSize,
                                           UINT* puPageCnt) {
  if (puPageCnt == nullptr) {
    return E_INVALIDARG;
  }
  *puPageCnt = 1;  // 单页（候选数量有限，不分页）
  if (pIndex != nullptr) {
    if (uSize < *puPageCnt) {
      return E_INVALIDARG;
    }
    *pIndex = 0;
  }
  return S_OK;
}

STDMETHODIMP CandidateListUI::SetPageIndex(UINT* pIndex, UINT uPageCnt) {
  (void)uPageCnt;  // 单页，忽略页数
  if (pIndex == nullptr) {
    return E_INVALIDARG;
  }
  return S_OK;
}

STDMETHODIMP CandidateListUI::GetCurrentPage(UINT* puPage) {
  if (puPage == nullptr) {
    return E_INVALIDARG;
  }
  *puPage = 0;
  return S_OK;
}

STDMETHODIMP CandidateListUI::SetSelection(UINT nIndex) {
  // UWP 应用高亮候选（点击/方向键移动）——记录；实际选择走按键（见 OnKeyDown）
  if (nIndex < texts_.size()) {
    highlighted_ = nIndex;
  }
  return S_OK;
}

STDMETHODIMP CandidateListUI::Finalize() {
  EndUI();
  return S_OK;
}

STDMETHODIMP CandidateListUI::Abort() {
  // UWP 用户取消候选（Esc/点击外部）：通知 text_service 中止组合
  if (abort_cb_) {
    abort_cb_();
  }
  EndUI();
  return S_OK;
}

STDMETHODIMP CandidateListUI::SetIntegrationStyle(GUID guidIntegrationStyle) {
  (void)guidIntegrationStyle;  // 集成样式由应用决定，本类不做额外处理
  return S_OK;
}

STDMETHODIMP CandidateListUI::GetSelectionStyle(
    TfIntegratableCandidateListSelectionStyle* ptfSelectionStyle) {
  if (ptfSelectionStyle == nullptr) {
    return E_INVALIDARG;
  }
  *ptfSelectionStyle = STYLE_ACTIVE_SELECTION;
  return S_OK;
}

STDMETHODIMP CandidateListUI::OnKeyDown(WPARAM wParam, LPARAM lParam,
                                        BOOL* pfEaten) {
  (void)wParam;
  (void)lParam;
  if (pfEaten == nullptr) {
    return E_INVALIDARG;
  }
  // UWP 内候选列表按键：交给 TSF 键事件链路（text_service 已接键盘 sink），
  // 此处不吞键，避免破坏空格/数字选字等正常输入
  *pfEaten = FALSE;
  return S_OK;
}

STDMETHODIMP CandidateListUI::ShowCandidateNumbers(BOOL* pfShow) {
  if (pfShow == nullptr) {
    return E_INVALIDARG;
  }
  *pfShow = TRUE;
  return S_OK;
}

STDMETHODIMP CandidateListUI::FinalizeExactCompositionString() {
  return E_NOTIMPL;  // 与 weasel 一致：不实现精确组合串终结
}

void CandidateListUI::StartUI() {
  if (ui_started_ || thread_mgr_ == nullptr) {
    return;
  }
  ITfUIElementMgr* mgr = nullptr;
  if (FAILED(thread_mgr_->QueryInterface(
          IID_ITfUIElementMgr, reinterpret_cast<void**>(&mgr))) ||
      mgr == nullptr) {
    return;
  }
  BOOL show = TRUE;
  if (SUCCEEDED(mgr->BeginUIElement(this, &show, &ui_element_id_))) {
    ui_started_ = true;
    // BeginUIElement 的 pbShow 已请求显示；TSF 决定实际显示与否
    //（ITfUIElementMgr 无 ShowUIElement 方法，weasel 同款只 Update）
  }
  mgr->Release();
}

void CandidateListUI::EndUI() {
  if (!ui_started_) {
    return;
  }
  if (thread_mgr_ != nullptr) {
    ITfUIElementMgr* mgr = nullptr;
    if (SUCCEEDED(thread_mgr_->QueryInterface(
            IID_ITfUIElementMgr, reinterpret_cast<void**>(&mgr))) &&
        mgr != nullptr) {
      mgr->EndUIElement(ui_element_id_);
      mgr->Release();
    }
  }
  ui_started_ = false;
  ui_element_id_ = 0;
}

void CandidateListUI::UpdateUIElement() {
  if (!ui_started_ || thread_mgr_ == nullptr) {
    return;
  }
  ITfUIElementMgr* mgr = nullptr;
  if (SUCCEEDED(thread_mgr_->QueryInterface(
          IID_ITfUIElementMgr, reinterpret_cast<void**>(&mgr))) &&
      mgr != nullptr) {
    mgr->UpdateUIElement(ui_element_id_);
    mgr->Release();
  }
}

#pragma warning(pop)

}  // namespace tsf
}  // namespace ime
