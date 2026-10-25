// 皮肤图片路径解析测试：验证 v4.5.3「复制到 skins\ 子目录」链路与候选窗加载无冲突。
//
// 能推翻的错误机制：
// 1. theme.yaml 写入时反斜杠未转义（\r \n 被 yaml 误读）→ 读回路径损坏
// 2. 加载时相对路径拼错基准（多拼/少拼 skins\ 一层）→ 候选窗找不到图
// 3. background_image / role_image / frame_image 三个字段路径解析不一致
// 4. 绝对路径原样保留（不被二次拼接）
//
// 用户担心点：settings_window 上传把图片复制到 config_dir\skins\、字段存相对路径
// "skins\role_x.png"；候选窗 ResolveSkinPath 拼接 config_dir + rel 加载。
// 本测试用公开 API（WriteThemeToYaml / LoadThemeFromYaml）实证该链路。

#include "skin/theme.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ime::skin;

static int g_failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                              \
    }                                                            \
  } while (0)

// Windows 路径分隔符统一成反斜杠便于比较（ResolveImagePath 用 filesystem 拼，
// 返回 native 格式 \；手动构造时也可能来 /）。
static std::string NormPath(const std::string& p) {
  std::string out;
  out.reserve(p.size());
  for (char c : p) {
    out.push_back(c == '/' ? '\\' : c);
  }
  return out;
}

int main() {
  // 临时目录：<temp>/beautype_skin_test/config/theme.yaml + skins\ 子目录
  std::filesystem::path tmp_root = std::filesystem::temp_directory_path() / "beautype_skin_test";
  std::filesystem::remove_all(tmp_root);
  const std::filesystem::path cfg_dir = tmp_root / "config";
  const std::filesystem::path skins_dir = cfg_dir / "skins";
  std::error_code ec;
  std::filesystem::create_directories(skins_dir, ec);
  CHECK(!ec);

  // 1) 写入侧：Theme.skin.role_image 存相对路径 skins\role_1.png（v4.5.3 上传后
  //    settings_window 写入的正是这种值），写出的 yaml 必须把反斜杠转义为 \\，
  //    否则 yaml 解析时 \r \n 被当转义序列 → 读回路径损坏。
  {
    Theme t = DefaultTheme();
    t.skin.mode = SkinMode::Role;
    t.skin.role_image = "skins\\role_1.png";
    t.skin.frame_image = "skins\\frame_2.png";
    t.background_image = "skins\\bg_3.png";  // background 同链路（旧 v1 语义）
    const std::string yaml = (cfg_dir / "theme.yaml").string();
    CHECK(WriteThemeToYaml(yaml, t));

    // 读原始 yaml 文本：反斜杠必须成对出现（EscapeYamlString）
    std::ifstream in(yaml, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(raw.find("skins\\\\role_1.png") != std::string::npos);   // 写出的 yaml 含转义
    CHECK(raw.find("skins\\\\frame_2.png") != std::string::npos);
    CHECK(raw.find("skins\\\\bg_3.png") != std::string::npos);
  }

  // 2) 读取侧：yaml 里的相对路径 skins\xxx.png 必须以 theme.yaml 所在目录（config\）
  //    为基准拼接成 config\skins\xxx.png（ResolveImagePath：base = yaml parent）。
  //    这正是候选窗 ResolveSkinPath 能加载到文件的前提。
  {
    Theme t;
    CHECK(LoadThemeFromYaml((cfg_dir / "theme.yaml").string(), &t));
    const std::string want_role = NormPath((cfg_dir / "skins" / "role_1.png").string());
    const std::string want_frame = NormPath((cfg_dir / "skins" / "frame_2.png").string());
    const std::string want_bg = NormPath((cfg_dir / "skins" / "bg_3.png").string());
    CHECK(NormPath(t.skin.role_image) == want_role);
    CHECK(NormPath(t.skin.frame_image) == want_frame);
    CHECK(NormPath(t.background_image) == want_bg);
  }

  // 3) 绝对路径原样保留（不二次拼接）——用户手写绝对路径场景
  {
    const std::string abs_role = NormPath((tmp_root / "absolute_role.png").string());
    Theme t = DefaultTheme();
    t.skin.mode = SkinMode::Role;
    t.skin.role_image = abs_role;
    const std::string yaml = (cfg_dir / "theme_abs.yaml").string();
    CHECK(WriteThemeToYaml(yaml, t));
    Theme back;
    CHECK(LoadThemeFromYaml(yaml, &back));
    CHECK(NormPath(back.skin.role_image) == abs_role);
  }

  // 4) 空字段：不解析成目录路径，保持空串（候选窗对空 = 纯色/无角色图）
  {
    Theme t = DefaultTheme();
    t.skin.mode = SkinMode::Role;
    t.skin.role_image = "";
    const std::string yaml = (cfg_dir / "theme_empty.yaml").string();
    CHECK(WriteThemeToYaml(yaml, t));
    Theme back;
    CHECK(LoadThemeFromYaml(yaml, &back));
    CHECK(back.skin.role_image.empty());
  }

  // 5) skins\ 子目录不存在时：加载仍成功（路径拼接不依赖文件存在，
  //    文件存在性由候选窗 ResolveSkinPath 加载 Bitmap 时才探测）
  {
    std::filesystem::remove_all(skins_dir, ec);
    Theme t;
    CHECK(LoadThemeFromYaml((cfg_dir / "theme.yaml").string(), &t));
    const std::string want_role = NormPath((cfg_dir / "skins" / "role_1.png").string());
    CHECK(NormPath(t.skin.role_image) == want_role);
  }

  std::filesystem::remove_all(tmp_root, ec);

  if (g_failures == 0) {
    std::printf("test_skins_path: ALL PASS\n");
    return 0;
  }
  std::printf("test_skins_path: %d FAILURE(S)\n", g_failures);
  return 1;
}
