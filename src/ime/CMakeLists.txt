# 输入法前端（M1）：引擎封装 / 候选窗 / 皮肤管线 / TSF 文本服务。
# - RIME_BEAUTIFY_BUILD_IME=ON（默认）：需要 librime（根 CMakeLists 已 add_subdirectory
#   rime-static）与 vcpkg（yaml-cpp），构建完整前端 + 3 个 ctest 冒烟。
# - OFF：骨架模式，仅构建 main 桩。
if(NOT RIME_BEAUTIFY_BUILD_IME)
  add_executable(rime_ime_skeleton main_skeleton.cpp)
  target_compile_features(rime_ime_skeleton PRIVATE cxx_std_17)
  if(MSVC)
    target_compile_options(rime_ime_skeleton PRIVATE /W4 /WX)
  endif()
  return()
endif()

# 源码内 include 均为带模块前缀路径（"skin/theme.h" / "engine/rime_engine.h" 等），
# 因此各库统一 PUBLIC 暴露 src/ime 根目录，让库之间与测试可互相包含。
set(IME_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# x86 TSF 双架构构建（-DRIME_BEAUTIFY_BUILD_TSF_X86=ON）：
# 服务化后 rime_tsf 只链 IPC/skin/ui，无需 rime_engine（librime）。引擎留在
# 64 位 rime_server，32 位 DLL 经 IPC 管道连接（管道通信与位宽无关）。
# 故此处跳过 rime_engine / rime_server / 测试，只构建 TSF 所需库与 DLL。

# --- 统一日志句柄（ime::g_log 定义点） ---
# 各宿主（rime_server/rime_tsf/测试）经依赖传递获得本库；宿主负责 fopen 赋值
# （server_main → rime_server.log，tsf_dll → rime_tsf.log，测试保持 nullptr）。
add_library(rime_log STATIC log/log.cpp)
target_include_directories(rime_log PUBLIC ${IME_INCLUDE_DIR})
target_compile_features(rime_log PUBLIC cxx_std_17)
# 发布构建开关：-DRIME_BEAUTIFY_RELEASE=ON 时宿主日志默认 INFO 起（对齐官方安静，
# 每键/hex dump 等 DEBUG 行不落盘）；测试构建默认 DEBUG 全量。
# 运行时可被 config\log_level 文本覆盖（排障写 0 重启即全量复现）。
option(RIME_BEAUTIFY_RELEASE "发布构建：宿主日志默认 INFO 起" OFF)
if(RIME_BEAUTIFY_RELEASE)
  target_compile_definitions(rime_log PUBLIC BEAUTYPE_RELEASE)
endif()

# --- 运行时界面语言表（中文/英文，单安装包运行时切换；不依赖 librime/yaml-cpp） ---
add_library(rime_i18n STATIC i18n/strings.cpp)
target_include_directories(rime_i18n PUBLIC ${IME_INCLUDE_DIR})
target_compile_features(rime_i18n PUBLIC cxx_std_17)

# --- 引擎封装（链接 rime-static） ---
# rime_api.h 在 librime 的 src/ 下；build_config.h 等生成头在 rime-build/src 下
# （librime 自身的 include_directories 只在其子树内生效，须在这里显式补上）。
# x86 TSF 构建（RIME_BEAUTIFY_BUILD_TSF_X86=ON）不构建引擎：见文件头部注释。
if(NOT RIME_BEAUTIFY_BUILD_TSF_X86)
add_library(rime_engine STATIC engine/rime_engine.cpp)
target_include_directories(rime_engine PUBLIC ${IME_INCLUDE_DIR}
  PRIVATE ${LIBRIME_SOURCE_DIR}/src ${CMAKE_BINARY_DIR}/rime-build/src)
target_link_libraries(rime_engine PRIVATE rime-static PUBLIC rime_log)
target_compile_features(rime_engine PUBLIC cxx_std_17)
endif()

# --- 皮肤管线（theme.yaml 解析用 yaml-cpp，vcpkg 提供 config 目标） ---
find_package(yaml-cpp CONFIG REQUIRED)
add_library(rime_skin STATIC
  skin/theme.cpp
  skin/theme_compiler.cpp
  skin/skin_importer.cpp
  skin/skin_applier.cpp)
target_include_directories(rime_skin PUBLIC ${IME_INCLUDE_DIR})
target_link_libraries(rime_skin PRIVATE yaml-cpp::yaml-cpp PUBLIC rime_log)
target_compile_features(rime_skin PUBLIC cxx_std_17)

# --- 候选窗（只消费 skin::Theme，不感知皮肤来源） ---
# gdiplus：PNG 背景图渲染（GDI+ 加载 background_image，铺候选窗背景）
add_library(rime_ui STATIC ui/candidate_window.cpp)
target_include_directories(rime_ui PUBLIC ${IME_INCLUDE_DIR})
target_link_libraries(rime_ui PRIVATE rime_skin gdiplus PUBLIC rime_log)
target_compile_features(rime_ui PUBLIC cxx_std_17)

# --- TSF 文本服务（引擎 + 候选窗装配） ---
# tsf_guid.cpp：CLSID/Profile 身份 GUID 定义（text_service 的 IsEqualCLSID 判定与
# rime_tsf.dll 的 DllGetClassObject 共用；test_tsf 链接 rime_frontend 即可解析）
add_library(rime_frontend STATIC tsf/text_service.cpp tsf/candidate_list.cpp
            tsf/language_bar.cpp tsf/tsf_guid.cpp)
target_include_directories(rime_frontend PUBLIC ${IME_INCLUDE_DIR})
# 服务化：text_service 经官方 Client（rime_ipc）连服务进程，不再引本地引擎
target_link_libraries(rime_frontend PRIVATE rime_ipc rime_skin rime_ui rime_i18n)
target_compile_features(rime_frontend PUBLIC cxx_std_17)

# --- 官方 WeaselIPC（服务化：DLL⇄服务进程 管道通信） ---
# 直接编译官方 weasel 源码（ActionLoader/Committer/ContextUpdater/Deserializer/
# PipeChannel/ResponseParser/Styler/WeaselClientImpl），协议 100% 与官方一致，
# 不自己发明。Boost 为官方 IPC 序列化依赖（纯头文件，third_party/boost）。
# WeaselUtility.cpp（getUsername/is_wow64 等）随 IPC 一并编译。
set(WEASEL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/weasel)
# Boost 官方 IPC 序列化依赖（纯头文件）。注意：
# 1) include 根必须是 boost 目录的父目录（源码写 #include <boost/...>）；
# 2) 不能放含中文的工程目录（cl 传参时中文路径损坏 → C1083），
#    故复制到纯 ASCII 路径 C:/boost_inc/（其下 boost/ 为 Boost 根）。
set(BOOST_INCLUDE_DIR C:/boost_inc)
# Boost 实现库：官方 b2 编译产物（官方命名 libboost_wserialization-vc143-mt-s-x64-1_87.lib
# 等，release static x64，见 C:/boost_src/boost_1_87_0/stage/lib）。由 Boost 官方
# auto-link 机制按名查找，这里只把目录交给链接器。
# x86 双架构构建时传入 -DBOOST_STAGE_LIB=C:/boost_src/boost_1_87_0/stage-x86/lib
# （b2 address-model=32 产物，库名后缀 -x32，auto-link 自动按位宽匹配）。
set(BOOST_STAGE_LIB "C:/boost_src/boost_1_87_0/stage/lib" CACHE PATH
    "Boost b2 静态库目录（x86 构建传 stage-x86/lib）")
link_directories(${BOOST_STAGE_LIB})
add_library(rime_ipc STATIC
  ${WEASEL_DIR}/WeaselIPC/ActionLoader.cpp
  ${WEASEL_DIR}/WeaselIPC/Committer.cpp
  ${WEASEL_DIR}/WeaselIPC/Configurator.cpp
  ${WEASEL_DIR}/WeaselIPC/ContextUpdater.cpp
  ${WEASEL_DIR}/WeaselIPC/Deserializer.cpp
  ${WEASEL_DIR}/WeaselIPC/PipeChannel.cpp
  ${WEASEL_DIR}/WeaselIPC/ResponseParser.cpp
  ${WEASEL_DIR}/WeaselIPC/Styler.cpp
  ${WEASEL_DIR}/WeaselIPC/WeaselClientImpl.cpp
  ${WEASEL_DIR}/RimeWithWeasel/WeaselUtility.cpp)
target_include_directories(rime_ipc PUBLIC ${IME_INCLUDE_DIR} ${WEASEL_DIR}/include
  ${BOOST_INCLUDE_DIR})
target_compile_features(rime_ipc PUBLIC cxx_std_17)

# --- 官方 WeaselIPCServer（服务进程端管道服务器） ---
add_library(rime_ipc_server STATIC
  ${WEASEL_DIR}/WeaselIPCServer/SecurityAttribute.cpp
  ${WEASEL_DIR}/WeaselIPCServer/WeaselServerImpl.cpp)
target_include_directories(rime_ipc_server PUBLIC ${IME_INCLUDE_DIR}
  ${WEASEL_DIR}/include ${BOOST_INCLUDE_DIR})
target_link_libraries(rime_ipc_server PRIVATE rime_ipc)
target_compile_features(rime_ipc_server PUBLIC cxx_std_17)

# --- Beautype 服务进程（照官方 WeaselServer 架构：管道服务 + 引擎 + 候选窗 + 托盘） ---
# x86 TSF 构建不生成服务进程：引擎服务保持 64 位，32 位 DLL 经 IPC 管道连接。
if(NOT RIME_BEAUTIFY_BUILD_TSF_X86)
# leveldb/glog/gflags 显式链接 vcpkg release 版（vcpkg 同名字 debug 库会被多配置
# 链接器误选 → _CrtDbgReport；glog 依赖 gflags → FlagRegisterer）。均为 vcpkg
# x64-windows-static 官方安装产物，release 路径，无自编。
set(VCPKG_STATIC_LIB C:/dev/vcpkg/installed/x64-windows-static/lib)
# settings_window：可视化皮肤设置面板（自绘圆角卡片 UI：GDI+；ChooseColor/
# ChooseFont/GetOpenFileName 通用对话框，官方用法见 ui/settings_window.h 头部注释；
# 链接 comdlg32.lib）。服务化后由托盘「设置」打开（服务进程内模态对话框，
# 保存后回调热更新候选窗）。
# WIN32 子系统：正式版不分配控制台窗口（黑框），日志走 rime_server.log 文件
# server/resource.rc：编译 IDI_BEAUTYPE 蝴蝶图标资源（对齐官方 WeaselServer
# 编译 WeaselServer.rc 的做法；设置窗口/托盘共用）
add_executable(rime_server WIN32 server/server_main.cpp server/beautype_handler.cpp
               server/pet_push_server.cpp ui/settings_window.cpp
               updater/updater.cpp server/resource.rc)
# 内部 target 名保持 rime_server（scripts/build-x64.cmd --target 不变），仅输出
# 文件名对齐官方「产品名+Server」（官方 WeaselServer.exe）→ BeautypeServer.exe。
set_target_properties(rime_server PROPERTIES OUTPUT_NAME BeautypeServer)
target_include_directories(rime_server PRIVATE ${IME_INCLUDE_DIR}
  ${WEASEL_DIR}/include ${BOOST_INCLUDE_DIR})
target_link_libraries(rime_server PRIVATE rime_engine rime_skin rime_ui
  rime_ipc_server rime_ipc rime_shared rime_i18n gdiplus shell32 shlwapi user32 ole32
  comctl32 comdlg32 ws2_32 ${VCPKG_STATIC_LIB}/leveldb.lib
  ${VCPKG_STATIC_LIB}/glog.lib ${VCPKG_STATIC_LIB}/gflags_static.lib
  winhttp urlmon)
# 产品版本号（自动更新版本比对用；打包时按版本改写，如 0.9.0 测试版 / 1.0.0 正式版）
target_compile_definitions(rime_server PRIVATE BEAUTYPE_VERSION="1.1.1")
# 诊断用 MAP 文件（定位崩溃地址符号归属；可随时移除）
target_link_options(rime_server PRIVATE "/MAP:BeautypeServer.map")
target_compile_features(rime_server PRIVATE cxx_std_17)
endif()

# --- TSF 系统级输入法（最终形态，M1.1）：COM DLL + TSF3 注册 ---
# 对照小狼毫（weasel）官方 TSF 组件：DllGetClassObject/DllRegisterServer 由
# regsvr32 调用注册（CTF TextInput 服务），系统经 CoCreateInstance 加载本 DLL，
# 任意应用内随光标弹出候选窗、无常驻演示窗口。导出符号经 rime_tsf.def
# （COM DLL 标准做法：不依赖 dllexport，避免与 SDK combaseapi.h 声明冲突）。
add_library(rime_tsf SHARED tsf/tsf_dll.cpp tsf/text_service.cpp
            tsf/candidate_list.cpp tsf/language_bar.cpp tsf/tsf_guid.cpp
            tsf/rime_tsf.def tsf/resource.rc)
target_include_directories(rime_tsf PUBLIC ${IME_INCLUDE_DIR})
# 服务化：链接官方 rime_ipc（Client/ResponseParser/PipeChannel），
# DLL 不再链接 rime_engine（引擎在服务进程）
target_link_libraries(rime_tsf PRIVATE rime_ipc rime_skin rime_ui rime_i18n uuid)
target_compile_features(rime_tsf PRIVATE cxx_std_17)
# 2026-10-14：DLL 输出名统一 beautype_tsf.dll（全程同名，对齐官方 weasel
# "安装目录名=系统目录名"惯例；此前构建产物 rime_tsf.dll 与注册/系统目录
# beautype_tsf.dll 双命名并存，导致 register_tip/打包偶发不匹配）
set_target_properties(rime_tsf PROPERTIES OUTPUT_NAME beautype_tsf)

# --- 冒烟测试（ctest） ---
# test_engine：librime 部署 + 模拟按键 "nihao" → 候选「你好」→ 空格上屏。
# test_ui：默认主题 + theme.yaml 解析 + 候选窗测量与离屏渲染。
# test_tsf：TSF 激活链路（①-A）+ 合成按键驱动引擎出候选/上屏（插入走 ①-B 待实测）。
# x86 TSF 构建跳过测试（无 librime 引擎，test_engine/test_tsf 依赖 rime_engine）。
if(RIME_BEAUTIFY_BUILD_TESTS AND NOT RIME_BEAUTIFY_BUILD_TSF_X86)
  add_executable(test_engine tests/test_engine.cpp)
  target_link_libraries(test_engine PRIVATE rime_engine)
  target_compile_features(test_engine PRIVATE cxx_std_17)
  add_test(NAME ime_engine_smoke COMMAND test_engine)

  add_executable(test_ui tests/test_ui.cpp)
  target_link_libraries(test_ui PRIVATE rime_ui rime_skin)
  target_compile_features(test_ui PRIVATE cxx_std_17)
  add_test(NAME ime_ui_smoke COMMAND test_ui)

  add_executable(test_tsf tests/test_tsf.cpp)
  target_link_libraries(test_tsf PRIVATE rime_frontend rime_ipc rime_engine rime_ui rime_skin)
  target_compile_features(test_tsf PRIVATE cxx_std_17)
  add_test(NAME ime_tsf_smoke COMMAND test_tsf)

  # CI 把预置数据目录经 configure 传入；测试经环境变量读取（本地可自行设置）
  if(RIME_SHARED_DATA_DIR)
    set_tests_properties(ime_engine_smoke ime_tsf_smoke
      PROPERTIES ENVIRONMENT "RIME_SHARED_DATA_DIR=${RIME_SHARED_DATA_DIR}")
  endif()
endif()

if(MSVC)
  set(_rime_w4_targets rime_log rime_i18n rime_skin rime_ui rime_frontend rime_tsf)
  if(NOT RIME_BEAUTIFY_BUILD_TSF_X86)
    list(APPEND _rime_w4_targets rime_engine test_engine test_ui test_tsf)
  endif()
  foreach(tgt ${_rime_w4_targets})
    target_compile_options(${tgt} PRIVATE /W4 /WX)
  endforeach()
  # 官方第三方源码不强制 /W4 /WX（避免官方警告打断构建）
  foreach(tgt rime_ipc rime_ipc_server)
    target_compile_options(${tgt} PRIVATE /W3)
  endforeach()
  # 对齐官方 WeaselTSF.vcxproj：/Zc:threadSafeInit-（关闭线程安全静态初始化）。
  # 官方显式加此选项（Release|Win32 AdditionalOptions）；Boost.Serialization 的
  # singleton 是静态局部变量，threadSafeInit 的 guard 机制在 DLL 卸载时有挂起问题。
  target_compile_options(rime_tsf PRIVATE /Zc:threadSafeInit-)
endif()

# --- 临时：设置窗口 UI 自测程序（验证后移除，不进 git 历史） ---
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../.debug_tmp/settings_dialog_test.cpp)
  add_executable(settings_ui_test WIN32 ${CMAKE_CURRENT_SOURCE_DIR}/../../.debug_tmp/settings_dialog_test.cpp
                 ui/settings_window.cpp)
  target_include_directories(settings_ui_test PRIVATE ${IME_INCLUDE_DIR})
  target_link_libraries(settings_ui_test PRIVATE rime_skin rime_ui rime_log
                        gdiplus comdlg32 user32 shell32 shlwapi comctl32)
  target_compile_features(settings_ui_test PRIVATE cxx_std_17)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../.debug_tmp/cand_probe.cpp)
  add_executable(cand_probe ${CMAKE_CURRENT_SOURCE_DIR}/../../.debug_tmp/cand_probe.cpp)
  target_include_directories(cand_probe PRIVATE ${IME_INCLUDE_DIR})
  target_link_libraries(cand_probe PRIVATE rime_skin rime_ui rime_log gdiplus)
  target_compile_features(cand_probe PRIVATE cxx_std_17)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../.debug_tmp/punct_probe.cpp)
  add_executable(punct_probe ${CMAKE_CURRENT_SOURCE_DIR}/../../.debug_tmp/punct_probe.cpp)
  target_include_directories(punct_probe PRIVATE ${IME_INCLUDE_DIR})
  target_link_libraries(punct_probe PRIVATE rime_engine)
  target_compile_features(punct_probe PRIVATE cxx_std_17)
endif()
