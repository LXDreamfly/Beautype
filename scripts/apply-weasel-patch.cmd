@echo off
setlocal
REM ============================================================
REM  apply-weasel-patch.cmd - Beautype local patch for official weasel
REM
REM  Patch content: add ImeActivated (IME activate/deactivate notify,
REM  TSF OnSetFocus -> pet show/hide). Official weasel has no such
REM  interface; Beautype needs it.
REM  Design record: docs/errors-and-lessons.md (ImeActivated).
REM  Re-applied and verified by x64 build on 2026-10-16 (exit=0).
REM
REM  Usage: scripts\apply-weasel-patch.cmd
REM  - If third_party\weasel missing: clone official weasel, pin commit
REM    d73f6295, apply patch.
REM  - If present: pin commit (keeps local changes) and apply patch
REM    idempotently (reverse-check first).
REM ============================================================

cd /d %~dp0..

if exist third_party\weasel\.git (
  echo [1/3] third_party\weasel exists, skip clone
) else (
  echo [1/3] cloning official weasel...
  git clone https://github.com/rime/weasel.git third_party\weasel
  if errorlevel 1 ( echo [ERR] clone failed & exit /b 1 )
)

echo [2/3] pinning official commit d73f6295 (patch generated from this commit)...
git -C third_party\weasel checkout d73f6295
if errorlevel 1 ( echo [ERR] checkout failed & exit /b 1 )

echo [3/3] applying patch patches\weasel-ime-activated.patch ...
git -C third_party\weasel apply --reverse --check ..\..\patches\weasel-ime-activated.patch
if errorlevel 1 (
  git -C third_party\weasel apply ..\..\patches\weasel-ime-activated.patch
  if errorlevel 1 ( echo [ERR] patch failed - weasel upstream may have changed, cross-check docs/errors-and-lessons.md & exit /b 1 )
  echo [OK] patch applied
) else (
  echo [OK] patch already applied, skip
)

echo [OK] weasel patched. Continue with scripts\build-x64.cmd
endlocal
