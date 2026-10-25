# 每个测试源文件独立可执行（各自自带 main），分别注册 ctest；
# 合并为单一可执行会导致 main 符号重复定义（LNK1169）。

add_executable(test_frame test_frame.cpp)
target_link_libraries(test_frame PRIVATE rime_shared)
target_compile_features(test_frame PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(test_frame PRIVATE /W4 /WX)
endif()
add_test(NAME test_frame COMMAND test_frame)

add_executable(test_protocol test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE rime_shared)
target_compile_features(test_protocol PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(test_protocol PRIVATE /W4 /WX)
endif()
add_test(NAME test_protocol COMMAND test_protocol)

# 皮肤图片路径解析：验证 v4.5.3「复制到 skins\ 子目录」上传链路与候选窗加载无冲突
# （yaml 反斜杠转义 → 读回拼接 config_dir\skins\xxx → 绝对路径原样保留 → 空字段容错）
add_executable(test_skins_path test_skins_path.cpp)
target_link_libraries(test_skins_path PRIVATE rime_skin)
target_compile_features(test_skins_path PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(test_skins_path PRIVATE /W4 /WX)
endif()
add_test(NAME test_skins_path COMMAND test_skins_path)
