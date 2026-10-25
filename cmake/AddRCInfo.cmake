# 兼容 shim（M1）：librime 的 src/CMakeLists.txt 在 WIN32 下以
#   include(${CMAKE_SOURCE_DIR}/cmake/AddRCInfo.cmake)
# 全路径引入其 MSVC 版本资源脚本（追加 rime.rc 到 rime_core_module_src）。
# 当 librime 作为本仓库的 add_subdirectory 子工程时，CMAKE_SOURCE_DIR
# 指向本仓库根目录，librime 找不到它自己的脚本 —— 这里转引 librime
# 的真实实现（LIBRIME_SOURCE_DIR 由根 CMakeLists 传入），行为与 librime
# 作为顶层项目构建时完全一致（含 rime.rc 版本资源与 git 分支探测）。
# 注意：本文件必须命名为 AddRCInfo.cmake 且位于仓库根 cmake/ 下，
# 与 librime 的 include 路径拼写完全一致。
include("${LIBRIME_SOURCE_DIR}/cmake/AddRCInfo.cmake")
