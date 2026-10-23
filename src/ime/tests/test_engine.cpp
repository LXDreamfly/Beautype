// CI 冒烟：librime 引擎「打字出候选、上屏」端到端。
// 数据目录经 RIME_SHARED_DATA_DIR 环境变量传入（ci.yml Configure 步骤设置），
// 本地可直接运行（需先 clone librime）。
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <windows.h>

#include "../fs_util.h"
#include "engine/rime_engine.h"

namespace {

std::string FindSharedDataDir() {
  if (const wchar_t* env = _wgetenv(L"RIME_SHARED_DATA_DIR"); env != nullptr && *env != L'\0') {
    return ime::WideToUtf8(env);
  }
  // 回退：常见 build 目录相对位置（build/ 与 build/x64/...）
  for (const char* rel : {"../../data/rime", "../../../data/rime",
                          "../../../../data/rime",
                          "../third_party/librime/data/minimal",
                          "../../third_party/librime/data/minimal",
                          "../../../third_party/librime/data/minimal"}) {
    std::error_code ec;
    if (ime::DirectoryExists(rel)) {
      return rel;
    }
  }
  return "";
}

std::filesystem::path MakeTempUserDir() {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("rime_beautify_engine_smoke_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

}  // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);  // 崩溃定位：无缓冲，逐行可见

  using namespace ime;
  std::printf("[test-engine] === M1 引擎冒烟：拼音 → 候选 → 上屏 ===\n");

  const std::string shared = FindSharedDataDir();
  if (shared.empty()) {
    std::printf("[test-engine] FAIL: 找不到预置数据目录（设置 RIME_SHARED_DATA_DIR）\n");
    return 1;
  }
  const auto user_dir = MakeTempUserDir();
  int failures = 0;

  RimeEngine engine;
  if (!engine.Initialize(shared, user_dir.string())) {
    std::printf("[test-engine] FAIL: Initialize/deploy\n");
    ++failures;
    return 1;
  }
  std::printf("[test-engine] librime %s 已部署\n", engine.version().c_str());

  const auto session = engine.CreateSession();
  if (session == 0) {
    std::printf("[test-engine] FAIL: CreateSession\n");
    ++failures;
  } else {
    engine.SelectSchema(session, "rime_ice");

    // 拼音 nihao（librime keysym 约定：字母一律小写，大小写经 shift 修饰位表达；
    // VK 码→keysym 的转换在 text_service 层，由 test_tsf 的真实链路覆盖）
    const int keys[] = {'n', 'i', 'h', 'a', 'o'};
    for (int k : keys) {
      engine.ProcessKey(session, k, 0);
    }
    if (engine.GetInput(session) != "nihao") {
      std::printf("[test-engine] FAIL: 组合串 != nihao（实际 %s）\n",
                  engine.GetInput(session).c_str());
      ++failures;
    }

    const std::vector<Candidate> cands = engine.GetCandidates(session);
    if (cands.empty()) {
      std::printf("[test-engine] FAIL: 无候选\n");
      ++failures;
    } else {
      std::printf("[test-engine] 候选 %zu 项，第 1 项=%s\n", cands.size(),
                  cands[0].text.c_str());
      bool found = false;
      for (const Candidate& c : cands) {
        if (c.text == "你好") {
          found = true;
          break;
        }
      }
      if (!found) {
        std::printf("[test-engine] FAIL: 候选中没有「你好」\n");
        ++failures;
      }
    }

    // 空格上屏高亮候选
    engine.ProcessKey(session, VK_SPACE, 0);
    const std::string commit = engine.GetCommit(session);
    std::printf("[test-engine] 上屏: %s\n", commit.empty() ? "(空)" : commit.c_str());
    if (commit != "你好") {
      std::printf("[test-engine] FAIL: 上屏 != 你好\n");
      ++failures;
    }
    engine.DestroySession(session);
  }

  // 标点直打：无 composition 时按 ？（XK_question 0x3F，Shift+/ 的 keysym）
  // → punctuator 的 half_shape '?' → 直接上屏全角 ？
  {
    const auto s = engine.CreateSession();
    if (s == 0) {
      std::printf("[test-engine] FAIL: 标点测试 CreateSession\n");
      ++failures;
    } else {
      engine.SelectSchema(s, "rime_ice");
      engine.ProcessKey(s, 0x003F, 0);
      const std::string commit = engine.GetCommit(s);
      std::printf("[test-engine] 标点 ？ 上屏: %s\n", commit.c_str());
      if (commit != "？") {
        std::printf("[test-engine] FAIL: 标点 ？ != ？\n");
        ++failures;
      }
      engine.DestroySession(s);
    }
  }

  // 方案切换：小鹤双拼（nihao → nihc）与五笔86（一 → gg）。
  // 同时证明 data/rime 三方案都部署成功（deploy 任一词典失败会在此前 Initialize 失败）。
  {
    const auto s = engine.CreateSession();
    if (s == 0) {
      std::printf("[test-engine] FAIL: 双拼测试 CreateSession\n");
      ++failures;
    } else {
      if (!engine.SelectSchema(s, "double_pinyin_flypy")) {
        std::printf("[test-engine] FAIL: SelectSchema(double_pinyin_flypy)\n");
        ++failures;
      } else {
        for (int k : {'n', 'i', 'h', 'c'}) {  // 小鹤双拼：ni + hc(hao)
          engine.ProcessKey(s, k, 0);
        }
        const std::vector<Candidate> cands = engine.GetCandidates(s);
        bool found = false;
        for (const Candidate& c : cands) {
          if (c.text == "你好") {
            found = true;
            break;
          }
        }
        if (!found) {
          std::printf("[test-engine] FAIL: 双拼候选中没有「你好」（当前方案=%s）\n",
                      engine.GetCurrentSchema(s).c_str());
          ++failures;
        }
        engine.ProcessKey(s, VK_SPACE, 0);
        if (engine.GetCommit(s) != "你好") {
          std::printf("[test-engine] FAIL: 双拼上屏 != 你好\n");
          ++failures;
        }
      }
      engine.DestroySession(s);
    }
  }

  {
    const auto s = engine.CreateSession();
    if (s == 0) {
      std::printf("[test-engine] FAIL: 五笔测试 CreateSession\n");
      ++failures;
    } else {
      if (!engine.SelectSchema(s, "wubi86")) {
        std::printf("[test-engine] FAIL: SelectSchema(wubi86)\n");
        ++failures;
      } else {
        for (int k : {'g'}) {  // 五笔86：「一」= 单码 g（最高权重）
          engine.ProcessKey(s, k, 0);
        }
        const std::vector<Candidate> cands = engine.GetCandidates(s);
        bool found = false;
        for (const Candidate& c : cands) {
          if (c.text == "一") {
            found = true;
            break;
          }
        }
        if (!found) {
          std::printf("[test-engine] FAIL: 五笔候选中没有「一」（当前方案=%s）\n",
                      engine.GetCurrentSchema(s).c_str());
          ++failures;
        }
        engine.ProcessKey(s, VK_SPACE, 0);
        if (engine.GetCommit(s) != "一") {
          std::printf("[test-engine] FAIL: 五笔上屏 != 一\n");
          ++failures;
        }
      }
      engine.DestroySession(s);
    }
  }

  // 退格语义实证（真机根因 2026-10-04：「退格删不了上屏的字」）。
  // 先确认 librime 对「空 composition 时的 BackSpace」返回 true（吃）还是
  // false（放行）——这决定退格删不了字是引擎吞键、还是宿主归还链路问题。
  // 注意：engine.ProcessKey 收 keysym（XK_BackSpace=0xFF08），非 VK 码。
  {
    const auto s = engine.CreateSession();
    if (s == 0) {
      std::printf("[test-engine] FAIL: 退格实证 CreateSession\n");
      ++failures;
    } else {
      engine.SelectSchema(s, "rime_ice");
      for (int k : {'n', 'i', 'h', 'a', 'o'}) {
        engine.ProcessKey(s, k, 0);
      }
      engine.ProcessKey(s, VK_SPACE, 0);
      const std::string committed = engine.GetCommit(s);
      std::printf("[test-engine] 退格实证：上屏=%s composition=%s\n",
                  committed.c_str(), engine.GetInput(s).c_str());

      // 场景 A：上屏后 composition 已空，按退格（librime 应放行 → 应用删字）
      const bool eaten_a = engine.ProcessKey(s, 0xFF08, 0);
      std::printf("[test-engine] 退格实证A（空组合退格）：eaten=%d input=%s\n",
                  static_cast<int>(eaten_a), engine.GetInput(s).c_str());

      // 场景 B：有拼音时退格（librime 应吃：删拼音尾）
      engine.ProcessKey(s, 'z', 0);
      const bool eaten_b = engine.ProcessKey(s, 0xFF08, 0);
      std::printf("[test-engine] 退格实证B（有拼音退格）：eaten=%d input=%s\n",
                  static_cast<int>(eaten_b), engine.GetInput(s).c_str());

      // 场景 C：拼音删空后再退格（回到空组合）
      const bool eaten_c = engine.ProcessKey(s, 0xFF08, 0);
      std::printf("[test-engine] 退格实证C（删空后再退格）：eaten=%d input=%s\n",
                  static_cast<int>(eaten_c), engine.GetInput(s).c_str());

      engine.DestroySession(s);
    }
  }

  engine.Finalize();


  // 部署健壮性（真机根因 2026-10-03）：user/build 产物被外部删除
  // （如清理旧配置目录）后，Initialize 必须检测到 schema 产物缺失 →
  // 清除安装标记强制全量重建 → 恢复可用（打字出候选）。
  {
    std::error_code ec;
    std::filesystem::remove_all(user_dir / "build", ec);
    std::filesystem::remove(user_dir / "installation.yaml", ec);
    std::printf("[test-engine] 已删除 user/build 与 installation.yaml，模拟产物缺失场景\n");

    RimeEngine engine2;
    if (!engine2.Initialize(shared, user_dir.string())) {
      std::printf("[test-engine] FAIL: 产物缺失后 Initialize（应自动强制重建）\n");
      ++failures;
    } else {
      std::printf("[test-engine] 产物缺失后 Initialize 自动重建成功\n");
      const auto s = engine2.CreateSession();
      if (s == 0) {
        std::printf("[test-engine] FAIL: 重建后 CreateSession\n");
        ++failures;
      } else {
        engine2.SelectSchema(s, "rime_ice");
        for (int k : {'n', 'i', 'h', 'a', 'o'}) {
          engine2.ProcessKey(s, k, 0);
        }
        bool ok = false;
        for (const Candidate& c : engine2.GetCandidates(s)) {
          if (c.text == "你好") {
            ok = true;
            break;
          }
        }
        if (!ok) {
          std::printf("[test-engine] FAIL: 重建后打字无候选\n");
          ++failures;
        } else {
          std::printf("[test-engine] 重建后打字出候选 OK（部署健壮性恢复）\n");
        }
        engine2.DestroySession(s);
      }
      engine2.Finalize();
    }
  }

  // 用户目录自愈（真机根因 2026-10-06）：user 目录残留与 shared 同名的旧方案源
  // （旧 rime_ice.dict.yaml、cn_dicts/ 等）会让 librime 用旧文件编译失败
  // （ERROR: dictionary 'rime_ice' failed to compile）。Initialize 必须先备份
  // 移走这些残留、再部署，且不删除用户数据（备份可审计）。
  {
    std::error_code ec;
    const std::filesystem::path dirty_dict = user_dir / "rime_ice.dict.yaml";
    {
      FILE* f = std::fopen(dirty_dict.string().c_str(), "w");
      if (f != nullptr) {
        std::fputs("name: rime_ice\nversion: \"0\"\n# 故意损坏的旧残留\n", f);
        std::fclose(f);
      }
    }
    const std::filesystem::path dirty_dir = user_dir / "cn_dicts";
    std::filesystem::create_directories(dirty_dir, ec);
    {
      FILE* f = std::fopen((dirty_dir / "fake.txt").string().c_str(), "w");
      if (f != nullptr) {
        std::fputs("fake", f);
        std::fclose(f);
      }
    }
    std::printf("[test-engine] 已预置脏残留（rime_ice.dict.yaml 坏文件 + cn_dicts/ 目录）\n");

    RimeEngine engine3;
    if (!engine3.Initialize(shared, user_dir.string())) {
      std::printf("[test-engine] FAIL: 脏残留下 Initialize（应自愈后成功）\n");
      ++failures;
    } else {
      std::printf("[test-engine] 脏残留下 Initialize 成功（自愈生效）\n");
      const bool cleaned = !std::filesystem::exists(dirty_dict, ec) &&
                           !std::filesystem::exists(dirty_dir, ec);
      bool backed_up = false;
      std::error_code bec;
      for (std::filesystem::directory_iterator it(user_dir, bec), end;
           it != end && !bec; it.increment(bec)) {
        if (it->path().filename().string().find("user_backup_sanitize_") == 0) {
          backed_up = true;
          break;
        }
      }
      if (!cleaned || !backed_up) {
        std::printf("[test-engine] FAIL: 残留未被自愈（cleaned=%d backed_up=%d）\n",
                    static_cast<int>(cleaned), static_cast<int>(backed_up));
        ++failures;
      } else {
        std::printf("[test-engine] 残留已备份移走 OK\n");
      }
      const auto s = engine3.CreateSession();
      if (s == 0) {
        std::printf("[test-engine] FAIL: 自愈后 CreateSession\n");
        ++failures;
      } else {
        engine3.SelectSchema(s, "rime_ice");
        for (int k : {'n', 'i', 'h', 'a', 'o'}) {
          engine3.ProcessKey(s, k, 0);
        }
        bool ok = false;
        for (const Candidate& c : engine3.GetCandidates(s)) {
          if (c.text == "你好") {
            ok = true;
            break;
          }
        }
        if (!ok) {
          std::printf("[test-engine] FAIL: 自愈后打字无候选\n");
          ++failures;
        } else {
          std::printf("[test-engine] 自愈后打字出候选 OK\n");
        }
        engine3.DestroySession(s);
      }
      engine3.Finalize();
    }
  }

  std::error_code ec;
  std::filesystem::remove_all(user_dir, ec);

  if (failures == 0) {
    std::printf("[test-engine] 结论：部署 + 拼音/标点/双拼/五笔 出候选与上屏 链路 OK\n");
    return 0;
  }
  std::printf("[test-engine] 结果：%d FAILURE(S)\n", failures);
  return 1;
}
