// 统一日志接口：ime::g_log 句柄 + 判空日志输出 LogF。
// 宿主（rime_server.exe → rime_server.log、rime_tsf.dll → rime_tsf.log）
// 负责 fopen 并赋值；测试/探针不打开（保持 nullptr），LogF 静默。
#pragma once

#include <cstdio>

namespace ime {

// 统一日志文件句柄（定义在 log/log.cpp；宿主 fopen 赋值）
extern FILE* g_log;

// 日志级别（业界对齐 glog：DEBUG < INFO < WARN < ERROR）。
// 过滤规则：行级别 < GetLogLevel() 的行不落盘。
enum LogLevel { kLogDebug = 0, kLogInfo = 1, kLogWarn = 2, kLogError = 3 };

// 当前有效级别：默认随构建走（未定义 BEAUTYPE_RELEASE = 测试全量 0；
// 发布 /DBEAUTYPE_RELEASE = 1 INFO 起）。宿主启动时可 SetLogLevel 覆盖
// （读 %APPDATA%\Beautype\config\log_level 文本文件，排障时写 0 全量复现）。
int GetLogLevel();
void SetLogLevel(int level);

// 判空日志输出：g_log 为 nullptr 或行级别被过滤时静默（任何二进制安全）。
// 格式化串必须是安全常量（与 fprintf 同语义）。行首自动补 [HH:MM:SS.mmm]。
void LogF(const char* fmt, ...);

}  // namespace ime
