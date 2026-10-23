#include "rime_engine.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <utility>

#include <rime_api.h>

#include "../fs_util.h"
#include "log/log.h"



namespace ime {

// 统一日志文件句柄：定义移至 log/log.cpp（rime_log 库），此处仅保留声明
// （engine/rime_engine.h: extern FILE* g_log）。宿主打开并赋值。
namespace {
RimeApi* Api() { return rime_get_api(); }

// 统一日志文件（g_log 跨翻译单元共享；正式版 WIN32 子系统无 stdout）
#define LOG(fmt, ...)                                                          \
  do {                                                                         \
    if (::ime::g_log != nullptr) ::ime::LogF( fmt, ##__VA_ARGS__); \
  } while (0)

// 部署产物硬校验：user/build/<schema>.schema.yaml 必须真实存在。
// （真机根因 2026-10-03：librime 的 deploy 会因 installation.yaml 已存在而判定
//  "已部署"跳过重建；若 user/build 产物被外部删除（清理旧配置目录等），引擎仍
//  启动但 schema 缺失 → 会话无候选、标点/上档全失效。）
bool SchemaBuildExists(const std::string& user_data_dir, const std::string& schema_id) {
  const auto p = std::filesystem::path(user_data_dir) / "build" / (schema_id + ".schema.yaml");
  std::error_code ec;
  return ime::FileExists(p.string());
}

// 清除安装标记 + build 缓存 → 下一次 api->deploy() 必定全量重建
// （librime 首次部署路径已实证：installation.yaml 缺失时完整执行
//  installation_update/workspace_update/schema_update 全部部署任务）。
void ForceRedeploy(const std::string& user_data_dir) {
  std::error_code ec;
  const auto build_dir = std::filesystem::path(user_data_dir) / "build";
  std::filesystem::remove_all(build_dir, ec);
  const auto inst = std::filesystem::path(user_data_dir) / "installation.yaml";
  std::filesystem::remove(inst, ec);
}

// 用户目录自愈（2026-10-06 真机根因修复）：
// librime 部署规则 = shared 数据只读、user 数据优先。若 user 目录残留与 shared
// 同名的方案源（旧版 rime_ice.dict.yaml / cn_dicts/ 等历史拷贝），librime 会用
// 旧文件编译 → "dictionary 'rime_ice' failed to compile"（14:39:08 ERROR 实锤）。
// 对策：初始化前扫描 user 顶层，凡是 shared 顶层存在同名条目（文件/目录）的
// 残留源，一律备份移走（不删除，保留可审计），让部署统一从 shared 进行。
// 保留项：user.yaml（用户设置）、installation.yaml（安装标记）、log/、build/。
void SanitizeUserDir(const std::string& shared_data_dir,
                     const std::string& user_data_dir) {
  std::error_code ec;
  const std::filesystem::path shared_p(shared_data_dir);
  const std::filesystem::path user_p(user_data_dir);
  if (!std::filesystem::is_directory(shared_p, ec) ||
      !std::filesystem::is_directory(user_p, ec)) {
    return;
  }
  std::filesystem::path backup;
  int moved = 0;
  std::error_code itec;
  for (std::filesystem::directory_iterator it(user_p, itec), end;
       it != end && !itec; it.increment(itec)) {
    const std::string name = it->path().filename().string();
    if (name == "user.yaml" || name == "installation.yaml" || name == "log" ||
        name == "build") {
      continue;
    }
    std::error_code sec;
    if (std::filesystem::exists(shared_p / name, sec)) {
      if (backup.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "user_backup_sanitize_%lld",
                      static_cast<long long>(std::time(nullptr)));
        backup = user_p / buf;
        std::filesystem::create_directories(backup, ec);
      }
      std::error_code mec;
      std::filesystem::rename(it->path(), backup / name, mec);
      if (!mec) {
        ++moved;
      }
    }
  }
  if (moved > 0) {
    LOG("[rime-engine] 用户目录自愈：移走 %d 个与共享数据同名的残留方案源 → %s\n",
        moved, backup.string().c_str());
  }
}
}  // namespace

RimeEngine::~RimeEngine() { Finalize(); }

bool RimeEngine::Initialize(const std::string& shared_data_dir,
                            const std::string& user_data_dir,
                            const std::string& schema_id) {
  if (initialized_) {
    Finalize();
  }
  RimeApi* api = Api();
  if (api == nullptr) {
    LOG("[rime-engine] FAIL: rime_get_api() 返回空\n");
    return false;
  }

  // 部署前诊断：shared/user 目录存在性（user 缺失是 main 未建目录的典型症状）
  {
    std::error_code ec;
    LOG("[rime-engine] shared 目录存在=%d，user 目录存在=%d\n",
        static_cast<int>(ime::DirectoryExists(shared_data_dir)),
        static_cast<int>(ime::DirectoryExists(user_data_dir)));
  }

  // 用户目录自愈：先于 librime setup，清掉与 shared 同名的残留方案源
  // （防止旧文件覆盖正确数据导致部署编译失败，见 SanitizeUserDir 注释）。
  LOG("[rime-engine] [debug] before SanitizeUserDir\n");
  SanitizeUserDir(shared_data_dir, user_data_dir);
  LOG("[rime-engine] [debug] after SanitizeUserDir\n");

  // librime 内部日志目录：user/log（glog 详细输出，部署失败时能看到内部原因）
  {
    std::error_code lec;
    log_dir_ = user_data_dir + "/log";
    std::filesystem::create_directories(log_dir_, lec);
  }
  LOG("[rime-engine] [debug] log_dir ready: %s\n", log_dir_.c_str());

  // librime 默认模块列表（kDefaultModules）只含 core/dict/gears；
  // levers 模块承载全部部署任务（installation_update 等），不显式加载则
  // api->deploy() 报 "unknown deployment task: installation_update"，
  // 部署失败 → build/default.yaml 缺失 → schema 加载失败。
  static const char* kBeautypeModules[] = {"core", "dict", "gears", "levers", nullptr};

  RIME_STRUCT(RimeTraits, traits);
  traits.app_name = "rime.beautify";
  traits.shared_data_dir = shared_data_dir.c_str();
  traits.user_data_dir = user_data_dir.c_str();
  traits.log_dir = log_dir_.c_str();
  // librime 引擎日志与宿主日志同级别联动：宿主全量(0) → 引擎 INFO 起落盘；
  // 宿主 ≥1（INFO/WARN/ERROR，即发布默认）→ 引擎只落 WARNING+（user\log 自动瘦身）
  traits.min_log_level = (GetLogLevel() >= kLogInfo) ? 1 : 0;
  traits.modules = kBeautypeModules;
  LOG("[rime-engine] [debug] before api->setup\n");
  api->setup(&traits);
  LOG("[rime-engine] [debug] after api->setup\n");
  api->initialize(&traits);
  LOG("[rime-engine] [debug] after api->initialize\n");

  // 诊断：共享数据可加载性（区分"数据解析问题"与"部署工作流问题"）
  {
    RimeConfig default_cfg = {};
    if (api->config_open("default", &default_cfg)) {
      LOG("[rime-engine] 共享 default.yaml 解析 OK\n");
      api->config_close(&default_cfg);
    } else {
      LOG("[rime-engine] FAIL: 共享 default.yaml 解析失败（数据损坏？）\n");
    }
    RimeSchemaList list = {};
    if (api->get_schema_list(&list)) {
      LOG("[rime-engine] schema 列表 %d 个:", static_cast<int>(list.size));
      for (int i = 0; i < list.size; ++i) {
        LOG(" %s", list.list[i].schema_id);
      }
      LOG("\n");
      api->free_schema_list(&list);
    } else {
      // 部署前 get_schema_list 不可用是预期：schema 列表要等 api->deploy()
      // 生成 user/build 后才就绪。此处仅诊断，不判失败（真正判据是下方
      // api->deploy() 的返回值），避免误导用户误以为引擎异常。
      LOG("[rime-engine] 部署前 get_schema_list 暂不可用（预期：deploy 生成 user/build 后可用）\n");
    }
  }

  // 同步部署：编译预置方案（.dict.yaml → table/prism）。失败则引擎不可用。
  if (!api->deploy()) {
    // 列出 shared 目录条目帮助定位（CI 冒烟同 minimal 数据可通过）
    std::string diag = "(none)";
    std::error_code ec;
    try {
      std::string list;
      int n = 0;
      for (const std::string& name : ime::ListDirectoryEntries(shared_data_dir)) {
        if (++n > 8) {
          break;
        }
        list += name;
        list += " ";
      }
      if (!list.empty()) {
        diag = list;
      }
    } catch (...) {
    }
    LOG("[rime-engine] FAIL: api->deploy() 失败（shared=%s 前几条: %s）\n",
        shared_data_dir.c_str(), diag.c_str());
    // 诊断：列出 user 目录部署产物（判断失败发生在哪一步：
    // 空目录=尚未开始/无写权限；只有 default.yaml=共享配置复制后失败；
    // build/ 有半成品=编译词典中途失败）
    {
      std::string user_diag = "(空，无任何产物)";
      std::error_code ec2;
      try {
        std::string list;
        int m = 0;
        for (const auto& e :
             std::filesystem::recursive_directory_iterator(user_data_dir, ec2)) {
          if (++m > 16) {
            break;
          }
          list += e.path().lexically_relative(user_data_dir).generic_string();
          list += " ";
        }
        if (!list.empty()) {
          user_diag = list;
        }
      } catch (...) {
      }
      LOG("[rime-engine] user 目录部署产物: %s\n", user_diag.c_str());
    }
    api->finalize();
    return false;
  }

  // 部署产物校验 + 强制重建（见 SchemaBuildExists 注释；一次重建仍缺失才判失败）
  if (!SchemaBuildExists(user_data_dir, schema_id)) {
    LOG("[rime-engine] 部署产物校验失败：%s.schema.yaml 缺失 → 清除安装标记强制重建…\n",
        schema_id.c_str());
    ForceRedeploy(user_data_dir);
    if (!api->deploy()) {
      LOG("[rime-engine] FAIL: 强制重建时 api->deploy() 失败\n");
      api->finalize();
      return false;
    }
    if (!SchemaBuildExists(user_data_dir, schema_id)) {
      LOG("[rime-engine] FAIL: 强制重建后 %s.schema.yaml 仍缺失，引擎不可用\n",
          schema_id.c_str());
      api->finalize();
      return false;
    }
    LOG("[rime-engine] 强制重建完成：%s.schema.yaml 已就绪\n", schema_id.c_str());
  }

  schema_id_ = schema_id;
  api_ = api;
  shared_data_dir_ = shared_data_dir;
  user_data_dir_ = user_data_dir;
  initialized_ = true;
  LOG("[rime-engine] initialized, librime %s, schema=%s\n", version().c_str(),
      schema_id_.c_str());
  return true;
}

bool RimeEngine::Redeploy() {
  if (!initialized_ || api_ == nullptr) {
    LOG("[rime-engine] Redeploy 跳过：引擎未初始化\n");
    return false;
  }
  RimeApi* api = Api();
  if (api == nullptr)
    return false;
  // 照官方 deployer_initialize + deploy（WeaselDeployer /deploy 语义）。
  // 部署期间调用方已进入维护模式（引擎不处理按键），会话保持（librime
  // deploy 不销毁既有会话，重新部署后即可继续输入）。
  RIME_STRUCT(RimeTraits, traits);
  traits.app_name = "rime.beautify";
  traits.shared_data_dir = shared_data_dir_.c_str();
  traits.user_data_dir = user_data_dir_.c_str();
  traits.log_dir = log_dir_.c_str();
  traits.min_log_level = 0;
  static const char* kBeautypeModules[] = {"core", "dict", "gears", "levers", nullptr};
  traits.modules = kBeautypeModules;
  api->deployer_initialize(&traits);
  const Bool ok = api->deploy();
  LOG("[rime-engine] Redeploy %s\n", ok ? "OK" : "FAILED");
  return ok != 0;
}

void RimeEngine::Finalize() {
  if (api_ != nullptr) {
    Api()->finalize();
  }
  api_ = nullptr;
  initialized_ = false;
}

std::string RimeEngine::version() const {
  const char* v = Api()->get_version();
  return v != nullptr ? v : "";
}

std::uint64_t RimeEngine::CreateSession() {
  return static_cast<std::uint64_t>(Api()->create_session());
}

void RimeEngine::DestroySession(std::uint64_t session) {
  if (session != 0) {
    Api()->destroy_session(static_cast<RimeSessionId>(session));
  }
}

bool RimeEngine::SelectSchema(std::uint64_t session, const std::string& schema_id) {
  return Api()->select_schema(static_cast<RimeSessionId>(session), schema_id.c_str()) != 0;
}

bool RimeEngine::ProcessKey(std::uint64_t session, int keycode, int mask) {
  return Api()->process_key(static_cast<RimeSessionId>(session), keycode, mask) != 0;
}

std::string RimeEngine::GetCommit(std::uint64_t session) {
  RimeApi* api = Api();
  RIME_STRUCT(RimeCommit, commit);
  if (!api->get_commit(static_cast<RimeSessionId>(session), &commit) || commit.text == nullptr) {
    return "";
  }
  std::string text = commit.text;
  api->free_commit(&commit);
  return text;
}

std::string RimeEngine::GetInput(std::uint64_t session) {
  const char* input = Api()->get_input(static_cast<RimeSessionId>(session));
  return input != nullptr ? input : "";
}

std::string RimeEngine::GetCompositionText(std::uint64_t session) {
  RimeApi* api = Api();
  RIME_STRUCT(RimeContext, ctx);
  std::string text;
  if (api->get_context(static_cast<RimeSessionId>(session), &ctx)) {
    if (ctx.composition.preedit != nullptr) {
      text = ctx.composition.preedit;
    }
    api->free_context(&ctx);
  }
  return text;
}

bool RimeEngine::GetPreeditCursor(std::uint64_t session, int* sel_start,
                                  int* sel_end, int* cursor_pos) const {
  if (sel_start == nullptr || sel_end == nullptr || cursor_pos == nullptr) {
    return false;
  }
  RimeApi* api = Api();
  RIME_STRUCT(RimeContext, ctx);
  if (!api->get_context(static_cast<RimeSessionId>(session), &ctx)) {
    return false;
  }
  // 官方字段：RimeComposition.sel_start/sel_end/cursor_pos（utf8 字节偏移；
  // librime src/rime_api.h RimeComposition）。组合不存在时 preedit 为空、
  // cursor_pos 恒 0——调用方以组合状态（IsComposing）为准。
  *sel_start = ctx.composition.sel_start;
  *sel_end = ctx.composition.sel_end;
  *cursor_pos = ctx.composition.cursor_pos;
  api->free_context(&ctx);
  return true;
}

std::vector<Candidate> RimeEngine::GetCandidates(std::uint64_t session) {
  RimeApi* api = Api();
  RIME_STRUCT(RimeContext, ctx);
  std::vector<Candidate> out;
  if (!api->get_context(static_cast<RimeSessionId>(session), &ctx)) {
    return out;
  }
  const int n = ctx.menu.num_candidates;
  out.reserve(static_cast<std::size_t>(n > 0 ? n : 0));
  for (int i = 0; i < n; ++i) {
    Candidate c;
    if (ctx.menu.candidates[i].text != nullptr) {
      c.text = ctx.menu.candidates[i].text;
    }
    if (ctx.menu.candidates[i].comment != nullptr) {
      c.comment = ctx.menu.candidates[i].comment;
    }
    out.push_back(std::move(c));
  }
  api->free_context(&ctx);
  return out;
}

int RimeEngine::GetHighlightedIndex(std::uint64_t session) {
  RimeApi* api = Api();
  RIME_STRUCT(RimeContext, ctx);
  int index = -1;
  if (api->get_context(static_cast<RimeSessionId>(session), &ctx)) {
    // 官方字段名：RimeMenu.highlighted_candidate_index（librime src/rime_api.h L137）
    index = ctx.menu.highlighted_candidate_index;
    api->free_context(&ctx);
  }
  return index;
}

std::string RimeEngine::GetCurrentSchema(std::uint64_t session) {
  // 官方 API：RimeApi->get_current_schema（librime src/rime_api.h L357），
  // RimeContext 本身不含 schema_id（schema_id 在 RimeStatus 中，经 get_status 获取）。
  char buf[128] = {0};
  if (Api()->get_current_schema(static_cast<RimeSessionId>(session), buf,
                                sizeof(buf))) {
    return std::string(buf);
  }
  return std::string();
}

bool RimeEngine::SelectCandidate(std::uint64_t session, std::size_t index) {
  return Api()->select_candidate(static_cast<RimeSessionId>(session), index) != 0;
}

void RimeEngine::SetOption(std::uint64_t session, const std::string& key, bool value) {
  if (Api()->set_option) {
    Api()->set_option(static_cast<RimeSessionId>(session), key.c_str(), value ? 1 : 0);
  }
}

bool RimeEngine::GetOption(std::uint64_t session, const std::string& key) {
  if (Api()->get_option) {
    return Api()->get_option(static_cast<RimeSessionId>(session), key.c_str()) != 0;
  }
  return false;
}

bool RimeEngine::IsComposing(std::uint64_t session) {
  // 官方语义：组合状态以 RimeStatus.is_composing 为准（RimeWithWeasel.cpp:761），
  // 不能用 preedit 非空推导（commit 后 preedit 已空但 is_composing 可能仍为真）。
  RimeApi* api = Api();
  RIME_STRUCT(RimeStatus, status);
  bool composing = false;
  if (api->get_status(static_cast<RimeSessionId>(session), &status)) {
    composing = !!status.is_composing;
    api->free_status(&status);
  }
  return composing;
}

void RimeEngine::ClearComposition(std::uint64_t session) {
  if (Api()->clear_composition) {
    Api()->clear_composition(static_cast<RimeSessionId>(session));
  }
}

void RimeEngine::CommitComposition(std::uint64_t session) {
  // 官方 RimeApi::commit_composition（rime_api.h:327）：提交当前组合。
  if (Api()->commit_composition) {
    Api()->commit_composition(static_cast<RimeSessionId>(session));
  }
}

std::vector<std::string> RimeEngine::GetSchemaList() {
  std::vector<std::string> result;
  RimeSchemaList list;
  if (Api()->get_schema_list(&list)) {
    for (std::size_t i = 0; i < list.size; ++i) {
      result.push_back(list.list[i].schema_id);
    }
    Api()->free_schema_list(&list);
  }
  return result;
}

}  // namespace ime
