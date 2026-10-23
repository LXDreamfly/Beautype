// 统一日志文件句柄（所有宿主二进制的 ime::g_log 定义点）+ 判空日志输出。
// 各宿主负责 fopen 并赋值：
//   - rime_server.exe → %APPDATA%\Beautype\rime_server.log（server_main.cpp）
//   - rime_tsf.dll     → %APPDATA%\Beautype\rime_tsf.log   （tsf_dll.cpp）
//   - 测试/探针无日志（保持 nullptr，LogF 静默，不会崩溃）
// 日志分级（业界对齐 glog）：行前缀白名单自动归类 DEBUG/INFO/WARN/ERROR，
// 行级别 < GetLogLevel() 不落盘。默认级别随构建：测试 0 全量 / 发布 1 INFO 起；
// 宿主启动时可 SetLogLevel 覆盖（读 config\log_level 文本，排障开 0）。
#include "log/log.h"

#include <windows.h>  // GetLocalTime / SYSTEMTIME（行首时间戳）

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace ime {

FILE* g_log = nullptr;

#ifndef BEAUTYPE_RELEASE
static const int kDefaultLogLevel = kLogDebug;  // 测试构建：全量
#else
static const int kDefaultLogLevel = kLogInfo;   // 发布构建：INFO 起
#endif
static int g_log_level = kDefaultLogLevel;

int GetLogLevel() { return g_log_level; }

void SetLogLevel(int level) {
  if (level >= kLogDebug && level <= kLogError) {
    g_log_level = level;
  }
}

// 行前缀白名单分级（覆盖 src/ime 全部 138 处 LOG/LogF 调用，逐行核对过）：
// [FAIL]/拉起失败 → ERROR；警告类/组件缺失/鉴权失败/预览失败/部署产物校验失败/
// candidate 定位两路全失败/解析失败/组合被宿主终止 → WARN；每键/每响应噪音
// （handler 的 ProcessKey/SyncUi/SendResponse、tsf 的按键/key→server/组合刷新/
// 属性/GUID_PROP/TestKeyDown/commit dump、candidate GetTextExt、rime-engine
// [debug] 探针）→ DEBUG；其余（生命周期/启动退出/用户动作/成功事件）→ INFO。
// 新增日志请沿用此前缀约定，级别自动归类。
static int LevelFromPrefix(const char* fmt) {
  // ERROR（功能失败）
  if (std::strncmp(fmt, "[FAIL]", 6) == 0) return kLogError;
  if (std::strncmp(fmt, "[tsf] 拉起失败", std::strlen("[tsf] 拉起失败")) == 0) return kLogError;
  // WARN（意外但已处理 / 回退 / 安全事件）
  if (std::strncmp(fmt, "[server] 警告", std::strlen("[server] 警告")) == 0) return kLogWarn;
  if (std::strncmp(fmt, "[server] 桌宠组件缺失", std::strlen("[server] 桌宠组件缺失")) == 0) return kLogWarn;
  if (std::strncmp(fmt, "[skin] 预览加载失败", std::strlen("[skin] 预览加载失败")) == 0) return kLogWarn;
  if (std::strncmp(fmt, "[skin] 预览路径", std::strlen("[skin] 预览路径")) == 0) return kLogWarn;
  if (std::strncmp(fmt, "[pet-push] 鉴权失败断开", std::strlen("[pet-push] 鉴权失败断开")) == 0) return kLogWarn;
  if (std::strncmp(fmt, "[rime-engine] 部署产物校验失败",
                   std::strlen("[rime-engine] 部署产物校验失败")) == 0) return kLogWarn;
  if (std::strncmp(fmt, "[candidate] server 定位 GetTextExt 两路全失败",
                   std::strlen("[candidate] server 定位 GetTextExt 两路全失败")) == 0) return kLogWarn;
  if (std::strncmp(fmt, "[tsf] GetResponseData 解析失败",
                   std::strlen("[tsf] GetResponseData 解析失败")) == 0) return kLogWarn;
  if (std::strncmp(fmt, "[tsf] 组合被宿主终止", std::strlen("[tsf] 组合被宿主终止")) == 0) return kLogWarn;
  // DEBUG（每键/每响应噪音与探针，测试全量可见、发布默认关）
  if (std::strncmp(fmt, "[debug]", 7) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[handler] SyncUi", std::strlen("[handler] SyncUi")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[handler] SendResponse", std::strlen("[handler] SendResponse")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[handler] ProcessKey", std::strlen("[handler] ProcessKey")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[tsf] 收到按键释放", std::strlen("[tsf] 收到按键释放")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[tsf] OnTestKeyDown", std::strlen("[tsf] OnTestKeyDown")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[tsf] 按键", std::strlen("[tsf] 按键")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[tsf] key→server", std::strlen("[tsf] key→server")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[tsf] 服务端未吃键", std::strlen("[tsf] 服务端未吃键")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[tsf] commit宽字符字节", std::strlen("[tsf] commit宽字符字节")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[tsf] 组合刷新", std::strlen("[tsf] 组合刷新")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[tsf] 属性", std::strlen("[tsf] 属性")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[tsf] GUID_PROP_COMPOSING", std::strlen("[tsf] GUID_PROP_COMPOSING")) == 0) return kLogDebug;
  if (std::strncmp(fmt, "[candidate] server GetTextExt", std::strlen("[candidate] server GetTextExt")) == 0) return kLogDebug;
  return kLogInfo;
}

void LogF(const char* fmt, ...) {
  if (g_log == nullptr) {
    return;
  }
  // 分级过滤：发布默认 kLogInfo(1) 起，DEBUG(0) 行不落盘（排障 SetLogLevel(0) 恢复全量）
  if (LevelFromPrefix(fmt) < g_log_level) {
    return;
  }
  // 行首时间戳 [HH:MM:SS.mmm]（真机 2026-10-13 引入，2026-10-18 重写）：
  // 追加模式（"a"）下 fseek/fgetc 回读"是否行首"实测仅首行生效，改为
  // 行缓冲 + 行首状态：每次调用先格式化，若处于行首则补时间戳，再按
  // 结尾是否 '\n' 更新状态——多行续行（如 schema 列表的 " %s" / "\n"
  // 独立调用）不误加时间戳。同时用临界区串行化：IME 日志被 handler
  // 线程/主线程并发写，此前无锁存在行撕裂风险，一并收敛。
  // 状态为进程级（server / tsf 宿主各自独立）；日志打开后首行必为行首。
  static CRITICAL_SECTION g_log_cs;
  static bool g_log_cs_ready = false;
  static bool g_at_line_start = true;
  if (!g_log_cs_ready) {
    InitializeCriticalSection(&g_log_cs);
    g_log_cs_ready = true;
  }
  EnterCriticalSection(&g_log_cs);
  char buf[4096];
  std::va_list args;
  va_start(args, fmt);
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n > 0) {
    if (g_at_line_start) {
      SYSTEMTIME st{};
      GetLocalTime(&st);
      char ts[32] = {};
      std::snprintf(ts, sizeof(ts), "[%02d:%02d:%02d.%03d] ", st.wHour,
                    st.wMinute, st.wSecond, st.wMilliseconds);
      std::fputs(ts, g_log);
    }
    std::fputs(buf, g_log);
    g_at_line_start = (buf[n - 1] == '\n');
    std::fflush(g_log);  // 常驻进程日志实时落盘
  }
  LeaveCriticalSection(&g_log_cs);
}

}  // namespace ime
