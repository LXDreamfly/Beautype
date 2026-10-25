cmake_minimum_required(VERSION 3.20)
project(rime_beautify LANGUAGES CXX)

# 测试开关必须在所有 add_subdirectory 之前 enable_testing()：
# enable_testing() 只对调用点之后添加的目录生效（CMake 目录作用域）。
# 历史教训：src/ime 在旧 enable_testing() 之前 add_subdirectory，导致
# ime_engine_smoke / ime_ui_smoke / ime_tsf_smoke 从未注册进 ctest——
# "CI 冒烟全绿"实际从未运行过这三个测试。
if(RIME_BEAUTIFY_BUILD_TESTS)
  enable_testing()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 组件开关：输入法 / 测试 / spike 探针 均可独立关闭
option(RIME_BEAUTIFY_BUILD_IME "构建输入法前端（M1 起：引擎/TSF/候选窗/皮肤管线）" ON)
option(RIME_BEAUTIFY_BUILD_TESTS "构建并注册单元测试" ON)
# x86 TSF 双架构支持（对齐官方小狼毫 weasel.dll x86 + weaselx64.dll x64 双 DLL）：
# 服务化后 rime_tsf 不链接 librime 引擎，32 位 TSF 组件只需 IPC/skin/ui。
# 开启时跳过 librime/测试/spike，只构建 rime_tsf + register_tip。
option(RIME_BEAUTIFY_BUILD_TSF_X86 "仅构建 x86 TSF 组件（32 位双架构支持）" OFF)
if(RIME_BEAUTIFY_BUILD_TSF_X86)
  set(RIME_BEAUTIFY_BUILD_TESTS OFF)
endif()

# MSVC：源码按 UTF-8 解释（仓库源码含中文串字面量，CI 为英文区域，缺省会按 CP1252
# 误读导致乱码/编译错）；抑制 CRT 弃用警告（getenv 等 C4996）。
# 运行时库统一为静态 /MT（spike-librime 第九轮链接期定案）：vcpkg x64-windows-static
# 依赖为 /MT，须在 add_subdirectory 前设置，让 librime 与本工程所有目标统一运行时库
# （避免 LNK2038 混链）。
if(MSVC)
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  # Windows 工程标准：定义 UNICODE 使 IDC_ARROW 等宏展开为 W 版（LPCWSTR）。
  # 源码全部显式调用 W 版 API（LoadCursorW/CreateWindowExW/...），无 TCHAR 用法。
  add_compile_definitions(UNICODE _UNICODE)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# 共享 IPC 协议库（帧编解码 / 端口文件；IME 与桌宠共用，tests/unit 依赖）
add_subdirectory(src/shared)

# --- librime 集成（M1） ---
# CI 先把 librime clone 到 <repo>/third_party/librime，再传 -DLIBRIME_SOURCE_DIR。
# 本地无 librime/vcpkg 时用 -DRIME_BEAUTIFY_BUILD_IME=OFF 只构建骨架。
set(LIBRIME_SOURCE_DIR "" CACHE PATH "librime 源码目录（主 CI 传入）")
if(RIME_BEAUTIFY_BUILD_IME)
  if(RIME_BEAUTIFY_BUILD_TSF_X86)
    # x86 TSF 组件不需要 librime（服务化：DLL 只链 IPC/skin/ui，引擎在 64 位 rime_server）。
    add_subdirectory(src/ime)
  else()
    if(NOT LIBRIME_SOURCE_DIR)
      message(FATAL_ERROR
        "RIME_BEAUTIFY_BUILD_IME=ON 需要 -DLIBRIME_SOURCE_DIR=<librime 源码绝对路径>\n"
        "（CI 中 clone 到 third_party/librime 后传入；本地无 librime 请用 -DRIME_BEAUTIFY_BUILD_IME=OFF）")
    endif()
    if(NOT IS_DIRECTORY "${LIBRIME_SOURCE_DIR}")
      message(FATAL_ERROR "LIBRIME_SOURCE_DIR 指向的目录不存在: ${LIBRIME_SOURCE_DIR}")
    endif()
    # librime 构建开关（FORCE 覆盖其默认值，与 spike-librime 定案一致）：
    # ENABLE_LOGGING=ON 启用 glog 内部日志（部署失败原因可见；vcpkg 装 glog）；
    # BUILD_SHARED_LIBS=OFF → 静态库目标 rime-static；BUILD_STATIC=ON →
    # YAML_CPP_STATIC_DEFINE / Opencc 静态宏（vcpkg 静态 triplet 依赖配套）。
    set(ENABLE_LOGGING ON CACHE BOOL "" FORCE)
    set(BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(BUILD_DATA OFF CACHE BOOL "" FORCE)
    set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC ON CACHE BOOL "" FORCE)
    # Boost 头文件（librime 在 Windows 需要 8 个 header-only 组件，FindBoost module
    # 模式不搜 vcpkg 目录）：显式把 vcpkg include 目录加入全局编译搜索路径。
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
      include_directories(SYSTEM "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
    endif()
    # vcpkg 多配置 VS 工程下 find_library 配置阶段默认搜到 debug 目录（同名库），
    # 导致 rime-static 链入 debug 版 glog/leveldb → Release 链接报 _CrtDbgReport。
    # 与官方 weasel 一致：显式把各库指到 vcpkg release 路径（不改 librime 源码）。
    set(_vcpkg_rel "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    set(LevelDb_LIBRARY "${_vcpkg_rel}/leveldb.lib" CACHE FILEPATH "" FORCE)
    set(Glog_LIBRARY "${_vcpkg_rel}/glog.lib" CACHE FILEPATH "" FORCE)
    set(Gflags_LIBRARY "${_vcpkg_rel}/gflags_static.lib" CACHE FILEPATH "" FORCE)
    set(Marisa_LIBRARY "${_vcpkg_rel}/marisa.lib" CACHE FILEPATH "" FORCE)
    set(Opencc_LIBRARY "${_vcpkg_rel}/opencc.lib" CACHE FILEPATH "" FORCE)
    set(YamlCpp_LIBRARY "${_vcpkg_rel}/yaml-cpp.lib" CACHE FILEPATH "" FORCE)
    add_subdirectory(${LIBRIME_SOURCE_DIR} rime-build)
    # glog/gflags 在 Windows 需要 dbghelp/shlwapi；经 rime-static 传播给最终链接，
    # 避免 LNK 错误（注意：必须用纯签名，librime 自身对 rime-static 也用纯签名）
    if(TARGET rime-static)
      target_link_libraries(rime-static dbghelp shlwapi)
    endif()
    add_subdirectory(src/ime)
  endif()
else()
  # 骨架模式：ime 只构建 main 桩（无 librime）
  add_subdirectory(src/ime)
endif()

if(RIME_BEAUTIFY_BUILD_TESTS)
  add_subdirectory(tests/unit)
endif()

# 启动器：双击同时打开输入法和桌宠
add_executable(beautype_launcher WIN32 src/launcher/main.cpp src/launcher/resource.rc)
target_compile_features(beautype_launcher PRIVATE cxx_std_17)
target_link_libraries(beautype_launcher PRIVATE user32)

# TSF 注册工具：照小狼毫官方（WeaselSetup/imesetup.cpp + WeaselTSF/Register.cpp）
# 实现 RegisterProfile + RegisterCategory（TF_CreateCategoryMgr 动态加载 msctf.dll）+
# EnableLanguageProfileByDefault + InstallLayoutOrTip，管理员运行，
# 把 Beautype 装进系统输入法布局。
add_executable(register_tip src/setup/register_tip.cpp)
target_compile_features(register_tip PRIVATE cxx_std_17)
target_link_libraries(register_tip PRIVATE uuid)

# BeautypeDeployer.exe —— 对齐官方 WeaselDeployer.exe，独立进程跑部署/设置/同步
add_executable(deployer WIN32 src/setup/deployer.cpp src/ime/ui/settings_window.cpp)
# 输出名统一 BeautypeDeployer.exe：server LaunchDeployer 按此名拉起（server_main.cpp:622），
# 曾因目标名 deployer 输出 deployer.exe 与 server 查找名错位 → 设置/重新部署/同步三项静默失败
# （m1 §29，2026-10-15 本机 rime_server.log 5x FAIL: BeautypeDeployer.exe 不存在）
set_target_properties(deployer PROPERTIES OUTPUT_NAME BeautypeDeployer)
target_compile_features(deployer PRIVATE cxx_std_17)
target_include_directories(deployer PRIVATE
  ${IME_INCLUDE_DIR}
  ${WEASEL_DIR}/include ${BOOST_INCLUDE_DIR}
  ${LIBRIME_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/rime-build/src)
target_link_libraries(deployer PRIVATE rime_engine rime_skin rime_ui rime_ipc
  rime_log rime_i18n gdiplus shell32 shlwapi user32 ole32 comctl32 comdlg32
  ${_vcpkg_rel}/leveldb.lib ${_vcpkg_rel}/glog.lib
  ${_vcpkg_rel}/gflags_static.lib)
# settings_window/candidate_window 直接用 boost wserialization（text_woarchive）；
# link_directories 在 src/ime/CMakeLists.txt 里，deployer 在根 CMakeLists 看不到，
# 这里显式补 boost stage lib 目录（与 src/ime/CMakeLists.txt:86 同一路径）。
target_link_directories(deployer PRIVATE C:/boost_src/boost_1_87_0/stage/lib)
target_link_options(deployer PRIVATE "/MAP:deployer.map")
