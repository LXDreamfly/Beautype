@echo off
setlocal
REM ============================================================
REM  get-librime.cmd - Fetch librime source for Beautype build
REM
REM  Beautype does not vendor librime source (BSD-3-Clause).
REM  This script clones the official repo and pins the commit
REM  the project was built against, so a fresh clone of Beautype
REM  can reproduce the build.
REM
REM  Pinned commit: 7bc3fb0005a03aff1c086611e4d52bb7f6444fdd
REM  Official repo: https://github.com/rime/librime.git
REM  License: BSD-3-Clause (see upstream LICENSE)
REM
REM  Usage: scripts\get-librime.cmd
REM  - If third_party\librime missing: clone official librime.
REM  - If present: checkout the pinned commit (idempotent).
REM  - Verifies HEAD hash before exiting 0.
REM ============================================================

cd /d %~dp0..

if exist third_party\librime\CMakeLists.txt (
  echo [1/3] third_party\librime exists, skip clone
) else (
  echo [1/3] cloning official librime...
  git clone https://github.com/rime/librime.git third_party\librime
  if errorlevel 1 ( echo [ERR] clone failed & exit /b 1 )
)

echo [2/3] pinning official commit 7bc3fb0...
git -C third_party\librime checkout 7bc3fb0005a03aff1c086611e4d52bb7f6444fdd
if errorlevel 1 ( echo [ERR] checkout failed & exit /b 1 )

echo [3/3] verifying HEAD hash...
for /f %%i in ('git -C third_party\librime rev-parse HEAD') do set LIBRIME_HEAD=%%i
if "%LIBRIME_HEAD%"=="7bc3fb0005a03aff1c086611e4d52bb7f6444fdd" (
  echo [OK] librime pinned at 7bc3fb0. Continue with scripts\build-x64.cmd
) else (
  echo [ERR] librime HEAD mismatch: %LIBRIME_HEAD%
  exit /b 1
)
endlocal
