add_library(rime_shared STATIC
  ipc/frame.cpp
  ipc/protocol.cpp
)

target_include_directories(rime_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(rime_shared PUBLIC cxx_std_17)

if(MSVC)
  target_compile_options(rime_shared PRIVATE /W4 /WX)
endif()
