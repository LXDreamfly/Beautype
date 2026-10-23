// TSF 身份 GUID 定义（CLSID / Profile）。
// 供 rime_tsf.dll（DllGetClassObject/注册）与 rime_frontend（text_service 的
// IsEqualCLSID 判定）共用；test_tsf 链接 rime_frontend 时也能解析该符号。
#define INITGUID
#include <guiddef.h>

// ---- 身份常量（固定 GUID，注册表/代码一致，勿改） ----
// CLSID: B17E78C8-ABAF-4891-8441-42FC45F475A4
DEFINE_GUID(CLSID_BeautypeTSF, 0xB17E78C8, 0xABAF, 0x4891,
            0x84, 0x41, 0x42, 0xFC, 0x45, 0xF4, 0x75, 0xA4);
// Profile GUID: FC89E162-09DE-4842-A7C7-7F9BB73E888E（TSF3 简版注册可省略，备用）
DEFINE_GUID(IID_BeautypeProfile, 0xFC89E162, 0x09DE, 0x4842,
            0xA7, 0xC7, 0x7F, 0x9B, 0xB7, 0x3E, 0x88, 0x8E);
