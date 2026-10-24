; Beautype installation script (完全照官方 weasel install.nsi 写)
!include FileFunc.nsh
!include LogicLib.nsh
!include MUI2.nsh
!include x64.nsh
!include winVer.nsh

Unicode true

!define BEAUTYPE_VERSION 1.1.0
!define REG_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Beautype"
; 出厂默认桌宠皮肤文件名（与 src 三处 kDefaultRivName 约定一致；换默认动画时
; 把新 .riv 命名为 beautype_pet.riv 替换 packaging\Beautype\pet\ 下同名文件即可）
!define DEFAULT_RIV "beautype_pet.riv"

Name "Beautype ${BEAUTYPE_VERSION}"
OutFile "${__FILEDIR__}\..\..\dist\BeautypeSetup.exe"

InstallDir "$PROGRAMFILES\Beautype"
InstallDirRegKey HKLM "Software\Beautype" "InstallDir"

VIProductVersion "${BEAUTYPE_VERSION}.0"
VIAddVersionKey /LANG=2052 "ProductName" "Beautype"
VIAddVersionKey /LANG=2052 "Comments" "Beautype IME"
VIAddVersionKey /LANG=2052 "CompanyName" "Beautype"
; v4.7.7：补 LegalCopyright 元数据（对齐官方 output\install.nsi:34；消除 9100 警告）
VIAddVersionKey /LANG=2052 "LegalCopyright" "Copyright (C) 2026 Beautype"
VIAddVersionKey /LANG=2052 "FileDescription" "Beautype IME"
VIAddVersionKey /LANG=2052 "FileVersion" "${BEAUTYPE_VERSION}"

SetCompressor /SOLID lzma
RequestExecutionLevel admin

; v4.7.7：安装包图标用 Beautype 自有图标（对齐官方 install.nsi:38
; !define MUI_ICON ..\resource\weasel.ico；此 ico 由 gen_setup_icon.py 生成，
; 7 帧 16-256 全尺寸 32bpp）。未定义 MUI_UNICON，与官方一致（卸载器用 NSIS 默认图标）。
!define MUI_ICON "beautype-logo.ico"

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_NOAUTOCLOSE
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!define MUI_ABORTWARNING

; v4.7.7：对齐官方 install.nsi 宏顺序——官方 PAGE 宏（49-56 行）在前、
; MUI_LANGUAGE（62 行起）在后（MUI2 要求 LANGUAGE 在 PAGE 之后插入）；
; 原脚本 LANGUAGE 在 PAGE 之前，编译报页面顺序 warning（功能正常但非官方写法）。
!insertmacro MUI_LANGUAGE "SimpChinese"

; ---- v4.7.7：对齐官方 weasel install.nsi .onInit（115-195 行）升级检测 ----
; 官方：读旧版 UninstallString → 弹确认框 → 备份旧 data → 手动卸载旧版
; （不走旧 uninstall.exe，避免弹卸载 UI）→ 继续安装新版；安装段恢复备份。
; 备份只覆盖 data\rime 根层（对齐官方注释 "user files may exist"——用户可能
; 放进去的自定义方案文件；cn_dicts 等随包词典由新版安装包重新复制，官方同款语义）。
Function .onInit
  ReadRegStr $R0 HKLM "${REG_UNINST_KEY}" "UninstallString"
  StrCmp $R0 "" done

  StrCpy $0 "Upgrade"
  IfSilent uninst 0
  MessageBox MB_OKCANCEL|MB_ICONINFORMATION "安装前，请先卸载旧版本的 Beautype。$\n$\n点击「确定」移除旧版本，或点击「取消」放弃本次安装。" IDOK uninst
  Abort

uninst:
  ; Backup data directory from previous installation, user files may exist
  ReadRegStr $R1 HKLM "Software\Beautype" "InstallDir"
  StrCmp $R1 "" call_uninstaller
  IfFileExists $R1\data\rime\*.* 0 call_uninstaller
  CreateDirectory $TEMP\beautype-backup
  CopyFiles $R1\data\rime\*.* $TEMP\beautype-backup

call_uninstaller:
  ; v4.7.8 修复：升级路径 ExecWait /quit 卡死（根因：官方 WeaselServer 自始支持 /quit，
  ; 而 Beautype 早期自建服务 v4.6 及更早 wWinMain 无参数解析，旧版 exe 收到 /quit 直接以
  ; 服务模式常驻 → ExecWait 永不返回 → 安装器卡"正在安装"页）。方案 B：异步 Exec /quit
  ; （新版对齐官方，秒退）→ Sleep 2s 给优雅退出窗口 → nsExec taskkill /f 兜底强杀
  ; （旧版已常驻则被杀；新版已退则 taskkill 无目标无害）。
  ; 退出并结束服务进程：新名 BeautypeServer.exe 优先，兼容旧名 rime_server.exe
  ; （从旧版升级时旧进程常驻占文件锁，两个名都要 /quit + taskkill）
  IfFileExists "$R1\BeautypeServer.exe" 0 +2
  Exec '"$R1\BeautypeServer.exe" /quit'
  IfFileExists "$R1\rime_server.exe" 0 +2
  Exec '"$R1\rime_server.exe" /quit'
  Sleep 2000
  nsExec::Exec 'taskkill /f /im BeautypeServer.exe'
  nsExec::Exec 'taskkill /f /im rime_server.exe'
  ; v4.7.11：升级路径同样清理桌宠——旧版 pet_preview.exe 常驻会占用文件锁，
  ; 安装段 SetOverwrite try 复制新桌宠静默失败 → 残留旧 exe（带黑屏/右键菜单旧问题，
  ; 真机"覆盖安装后仍有旧问题"踩坑 2026-10-18；对齐卸载段 250-252 行做法）
  nsExec::Exec 'taskkill /f /im pet_preview.exe'
  Sleep 1000  ; 等 pet_preview.exe 文件锁释放（否则下方 RMDir /r "$R1\pet" 静默失败）
  ExecWait '"$R1\register_tip.exe" /u'
  ; Remove registry keys
  DeleteRegKey HKLM "Software\Beautype"
  DeleteRegKey HKLM "${REG_UNINST_KEY}"
  ; don't redirect on 64 bit system for auto run setting
  ${If} ${IsNativeARM64}
    SetRegView 64
  ${ElseIf} ${IsNativeAMD64}
    SetRegView 64
  ${Endif}
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "BeautypeServer"
  ; recover back to 32bit view
  SetRegView 32
  ; Remove files and uninstaller（data 树递归删净：cn_dicts/en_dicts/lua/opencc
  ; 子目录，官方枚举只覆盖两层，Beautype 目录深，/r 兜底防漏——卸载残留根因 12.1）
  RMDir /r "$R1\data"
  RMDir /r "$R1\pet"   ; v4.7.11：桌宠组件（pet_preview.exe + ${DEFAULT_RIV}），对齐卸载段 260 行
  Delete "$R1\x86\*.*"
  RMDir "$R1\x86"
  Delete "$R1\*.*"
  RMDir "$R1"
  SetShellVarContext all
  Delete "$SMPROGRAMS\Beautype\*.*"
  RMDir "$SMPROGRAMS\Beautype"
  ; Prompt reboot
  SetRebootFlag true
  Sleep 800

done:
FunctionEnd

Section "Beautype"
  SectionIn RO

  WriteRegStr HKLM "Software\Beautype" "InstallDir" "$INSTDIR"

  ; 退出并结束服务进程：新名 BeautypeServer.exe 优先，兼容旧名 rime_server.exe
  IfFileExists "$INSTDIR\BeautypeServer.exe" 0 +2
  Exec '"$INSTDIR\BeautypeServer.exe" /quit'
  IfFileExists "$INSTDIR\rime_server.exe" 0 +2
  Exec '"$INSTDIR\rime_server.exe" /quit'
  Sleep 2000
  nsExec::Exec 'taskkill /f /im BeautypeServer.exe'
  nsExec::Exec 'taskkill /f /im rime_server.exe'

  ; 对齐官方 install.nsi:217 SetOverwrite try
  SetOverwrite try
  SetOutPath $INSTDIR

  ; v4.7.7：恢复升级备份（对齐官方 install.nsi:221-224 weasel-backup 恢复）——
  ; .onInit 升级路径备份的 data\rime 根层用户文件恢复回新安装目录，然后清备份
  IfFileExists $TEMP\beautype-backup\*.* 0 program_files
  CreateDirectory "$INSTDIR\data\rime"
  CopyFiles $TEMP\beautype-backup\*.* "$INSTDIR\data\rime"
  RMDir /r $TEMP\beautype-backup

program_files:
  ; 复制文件到 $INSTDIR（对齐官方 install.nsi:227-281）
  ; 改名清理：旧版 rime_server.exe 实体删除，避免与新名 BeautypeServer.exe 并存
  Delete "$INSTDIR\rime_server.exe"
  File "beautype_tsf.dll"
  File "BeautypeServer.exe"
  File "register_tip.exe"
  File "BeautypeDeployer.exe"

  ; M4：桌宠组件（pet_preview.exe 单文件依赖系统 DLL + ${DEFAULT_RIV} 动画素材）。
  ; rime_server 启动后从注册表 InstallDir 拉起 pet\pet_preview.exe（见 server_main.cpp LaunchPetIfInstalled）。
  SetOutPath "$INSTDIR\pet"
  File "pet\pet_preview.exe"
  File "pet\${DEFAULT_RIV}"

  ; 复制 32 位 DLL 到 $INSTDIR\x86（对齐官方 install.nsi:240-242 weaselx64.dll）
  SetOutPath "$INSTDIR\x86"
  File "x86\beautype_tsf.dll"

  ; 复制 Rime 数据到 $INSTDIR\data（对齐官方 install.nsi:283-293）
  SetOutPath "$INSTDIR\data\rime"
  File /r "data\rime\*.*"

  SetOutPath $INSTDIR

  ; 调用 register_tip.exe 注册 TSF（完全照官方 install.nsi:297-308 WeaselSetup 参数：
  ; 默认 /i；安装器带 /S → /s；带 /T → /t）
  StrCpy $R2 "/i"
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/S" $R1
  IfErrors +2 0
  StrCpy $R2 "/s"
  ${GetOptions} $R0 "/T" $R1
  IfErrors +2 0
  StrCpy $R2 "/t"

  ; NSIS 无 $? 变量：用 nsExec::Exec 等待并弹出退出码（官方用 ExecWait 但不需要
  ; 检查结果；本注册器失败以退出码传播，故必须取码）。register_tip 为 GUI 子系统
  ; 无窗口，隐藏运行无行为差异。
  nsExec::Exec '"$INSTDIR\register_tip.exe" $R2'
  Pop $0

  ; v4.7.9：注册失败显式中止——register_tip 返回非 0 = 输入法注册失败
  ; （DLL 复制/regsvr32/RegisterProfile/RegisterCategories/EnableLanguageProfile/
  ; InstallLayoutOrTip/注册自检任一环节失败，register_tip.log 内记 REGISTER_TIP_FAILED）。
  ; 失败即中止安装：不写卸载项、不写 Run 键、不启动 rime_server——避免再次出现
  ; "安装流程正常走完、文件齐全、但输入法从未注册"的假成功（M1 复盘 §28）。
  ; 对齐官方：WeaselSetup 失败 Weasel 安装器同样中止并弹错。
  ; 注意：IntCmp v1 v2 eq lt gt 四分支——exit 码非 0（小于或大于）都进 reg_fail，
  ; 不能把 gt 分支写成 reg_ok（v4.7.9 首版写反导致失败仍继续安装，本机实测复现）。
  IntCmp $0 0 reg_ok reg_fail reg_fail
reg_fail:
  ; v4.7.10：失败弹窗给可操作指引（常见：杀毒软件拦截系统目录写入 / 旧 DLL 占用 /
  ; 未以管理员运行）。register_tip.log 内有 err 码与诊断行（真机对方 err=5 实测）。
  IfSilent abort_install
  MessageBox MB_OK|MB_ICONSTOP "Beautype 输入法注册失败。$\n$\n请打开安装目录下的 register_tip.log 查看失败原因。$\n常见原因与处理：$\n1. 杀毒软件拦截了系统目录写入（err=5）——请关闭实时防护或添加白名单后重试；$\n2. 旧版文件被占用（err=32）——请重启电脑后再装；$\n3. 未以管理员权限运行——请右键安装包选择「以管理员身份运行」。$\n$\n安装已中止。"
abort_install:
  Abort
reg_ok:

  ; 调用 BeautypeDeployer.exe 部署 Rime 数据（对齐官方 install.nsi:322-329 WeaselDeployer.exe；
  ; 输出名与 server LaunchDeployer 查找名统一，m1 §29）
  ; 非静默安装 → /install（首次部署）；静默安装 → /deploy。部署完成后再启动服务。
  IfSilent deploy_silently
  ExecWait '"$INSTDIR\BeautypeDeployer.exe" /install'
  GoTo deploy_done

  deploy_silently:
  ExecWait '"$INSTDIR\BeautypeDeployer.exe" /deploy'

  deploy_done:

  WriteRegStr HKLM "${REG_UNINST_KEY}" "DisplayName" "Beautype"
  WriteRegStr HKLM "${REG_UNINST_KEY}" "DisplayIcon" '"$INSTDIR\BeautypeServer.exe"'
  WriteRegStr HKLM "${REG_UNINST_KEY}" "DisplayVersion" "${BEAUTYPE_VERSION}"
  WriteRegStr HKLM "${REG_UNINST_KEY}" "UninstallString" '"$INSTDIR\uninst.exe"'
  WriteRegStr HKLM "${REG_UNINST_KEY}" "Publisher" "Beautype"
  WriteRegDWORD HKLM "${REG_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${REG_UNINST_KEY}" "NoRepair" 1
  WriteUninstaller "$INSTDIR\uninst.exe"

  ; 写自启动 Run 键（对齐官方 install.nsi:331-340 WriteRegStr Run WeaselServer，
  ; 重启电脑后 rime_server 自动启动）+ 启动服务
  ${If} ${IsNativeARM64}
    SetRegView 64
  ${ElseIf} ${IsNativeAMD64}
    SetRegView 64
  ${Endif}
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "BeautypeServer" "$INSTDIR\BeautypeServer.exe"
  ; Start BeautypeServer（对齐官方 Run WeaselServer + Exec 启动）
  Exec '"$INSTDIR\BeautypeServer.exe"'

  ; v4.7.7：升级路径提示重启（对齐官方 install.nsi:352-354 StrCmp $0 "Upgrade"）
  StrCmp $0 "Upgrade" 0 +2
  SetRebootFlag true

SectionEnd

; v4.7.7：开始菜单快捷方式（对齐官方 install.nsi:359-374 "Start Menu Shortcuts"，
; 可选 section 可取消）。【用户文件夹】→ rime_server.exe /userdir（官方
; LNKFORUSERFOLDER → WeaselServer.exe /userdir，打开 %APPDATA%\Beautype）。
Section "开始菜单快捷方式"
  SetShellVarContext all
  CreateDirectory "$SMPROGRAMS\Beautype"
  CreateShortCut "$SMPROGRAMS\Beautype\【Beautype】用户文件夹.lnk" "$INSTDIR\BeautypeServer.exe" "/userdir" "$SYSDIR\shell32.dll" 126
  CreateShortCut "$SMPROGRAMS\Beautype\卸载 Beautype.lnk" "$INSTDIR\uninst.exe" "" "$INSTDIR\uninst.exe" 0
SectionEnd

Section "Uninstall"

  ; 退出并结束服务进程：新名 BeautypeServer.exe 优先，兼容旧名 rime_server.exe
  IfFileExists "$INSTDIR\BeautypeServer.exe" 0 +2
  Exec '"$INSTDIR\BeautypeServer.exe" /quit'
  IfFileExists "$INSTDIR\rime_server.exe" 0 +2
  Exec '"$INSTDIR\rime_server.exe" /quit'
  Sleep 2000
  nsExec::Exec 'taskkill /f /im BeautypeServer.exe'
  nsExec::Exec 'taskkill /f /im rime_server.exe'
  ; M4+：桌宠进程也杀——否则 pet_preview.exe 被占用，RMDir /r "$INSTDIR\pet"
  ; 静默失败留下 exe+riv 残留（真机卸载验证踩坑 2026-10-14）
  nsExec::Exec 'taskkill /f /im pet_preview.exe'
  Sleep 1000  ; 等 pet_preview.exe 文件锁释放（taskkill 后 OS 释放句柄有延迟，
              ; 否则 RMDir /r "$INSTDIR\pet" 静默失败留 exe+riv 残留）

  ExecWait '"$INSTDIR\register_tip.exe" /u'

  ; 删文件（对齐官方 install.nsi:398-406 通配 Delete + RMDir；v4.7.7 修正：
  ; data 树用 RMDir /r 递归删净——cn_dicts/en_dicts/lua/opencc 子目录深，
  ; 官方枚举只覆盖两层会漏删（卸载残留根因 12.1，已在 docs/errors-and-lessons.md 记录））
  RMDir /r "$INSTDIR\data"
  RMDir /r "$INSTDIR\pet"   ; M4：桌宠组件（pet_preview.exe + ${DEFAULT_RIV}）
  Delete "$INSTDIR\x86\*.*"
  RMDir "$INSTDIR\x86"
  Delete "$INSTDIR\*.*"
  RMDir "$INSTDIR"

  ; 删除自启动 Run 键（对齐官方 install.nsi:169-177 DeleteRegValue Run WeaselServer）
  ${If} ${IsNativeARM64}
    SetRegView 64
  ${ElseIf} ${IsNativeAMD64}
    SetRegView 64
  ${Endif}
  DeleteRegValue HKLM "Software\Microsoft\Windows\CurrentVersion\Run" "BeautypeServer"
  ; 恢复 32 位注册表视图（对齐官方）
  SetRegView 32

  DeleteRegKey HKLM "${REG_UNINST_KEY}"
  DeleteRegKey HKLM "Software\Beautype"

  ; v4.7.7：删开始菜单快捷方式（对齐官方 install.nsi:407-409）
  SetShellVarContext all
  Delete "$SMPROGRAMS\Beautype\*.*"
  RMDir "$SMPROGRAMS\Beautype"

  SetRebootFlag true

SectionEnd
