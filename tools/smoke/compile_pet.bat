@echo off
rem Usage: compile_pet.bat [release]
rem   release -> adds /DBEAUTYPE_RELEASE (log default INFO+, quiet release build)
rem   no arg  -> test build (log default DEBUG 0, full detail)
if /I "%~1"=="release" (set EXTRA_DEFINES=/DBEAUTYPE_RELEASE) else (set EXTRA_DEFINES=)
if "%VCVARS%"=="" set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul 2>&1
cd /d "%~dp0..\.."
rem Macro set must match the premake vcxproj (lesson 16.4: without NOMINMAX,
rem windows.h min/max macros pollute rive headers -> cascading fake errors).
rem build_rive_clang.cmd uses: --with_rive_text --with_rive_layout
rem   --with_rive_canvas --with_rive_audio=system
rem => app macros MUST define WITH_RIVE_TEXT WITH_RIVE_LAYOUT RIVE_CANVAS
rem    WITH_RIVE_AUDIO MA_NO_RESOURCE_MANAGER, or class layout (vtable)
rem    diverges from rive.lib and heap corruption crashes follow (0xc0000374).
cl /O2 /EHsc /std:c++17 /utf-8 /W3 ^
  %EXTRA_DEFINES% ^
  /DWITH_RIVE_AUDIO /DWITH_RIVE_TEXT /DWITH_RIVE_LAYOUT /DRIVE_CANVAS /DMA_NO_RESOURCE_MANAGER /DNOMINMAX /DRIVE_WINDOWS /D_USE_MATH_DEFINES ^
  /I "%CD%\third_party\rive-runtime\include" ^
  /I "%CD%\third_party\rive-runtime\renderer\include" ^
  /I "%CD%\third_party\rive-runtime\tests\dependencies\rive-app_miniaudio_rive_changes_5" ^
  pet_preview.cpp ^
  /link /SUBSYSTEM:WINDOWS /OUT:pet_preview.exe ^
  %CD%\third_party\rive-runtime\tests\out\debug\rive.lib ^
  %CD%\third_party\rive-runtime\tests\out\debug\rive_pls_renderer.lib ^
  %CD%\third_party\rive-runtime\tests\out\debug\rive_decoders.lib ^
  %CD%\third_party\rive-runtime\tests\out\debug\rive_harfbuzz.lib ^
  %CD%\third_party\rive-runtime\tests\out\debug\rive_sheenbidi.lib ^
  %CD%\third_party\rive-runtime\tests\out\debug\rive_yoga.lib ^
  %CD%\third_party\rive-runtime\tests\out\debug\miniaudio.lib ^
  %CD%\third_party\rive-runtime\tests\out\debug\luau_vm.lib ^
  %CD%\third_party\rive-runtime\tests\out\debug\libjpeg.lib ^
  %CD%\third_party\rive-runtime\tests\out\debug\libpng.lib ^
  %CD%\third_party\rive-runtime\tests\out\debug\libwebp.lib ^
  %CD%\third_party\rive-runtime\tests\out\debug\zlib.lib ^
  d3d11.lib d3dcompiler.lib dxguid.lib gdiplus.lib gdi32.lib ws2_32.lib ole32.lib uuid.lib user32.lib shell32.lib comdlg32.lib
echo exit=%ERRORLEVEL%
