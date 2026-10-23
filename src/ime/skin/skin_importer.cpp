#include "skin_importer.h"

namespace ime::skin {

namespace {

class DefaultThemeImporter final : public SkinImporter {
 public:
  bool CanImport(const std::string& path) const override {
    return path.empty() || path == "default";
  }
  bool Import(const std::string&, Theme* out) const override {
    if (out == nullptr) return false;
    *out = DefaultTheme();
    return true;
  }
};

DefaultThemeImporter g_default_importer;
std::vector<const SkinImporter*> g_importers;

}  // namespace

void RegisterSkinImporter(const SkinImporter* importer) {
  if (importer != nullptr) {
    g_importers.push_back(importer);
  }
}

const SkinImporter* DefaultImporter() { return &g_default_importer; }

bool ImportSkin(const std::string& path, Theme* out) {
  // 内置默认主题始终可用
  if (g_default_importer.CanImport(path)) {
    return g_default_importer.Import(path, out);
  }
  for (const SkinImporter* importer : g_importers) {
    if (importer->CanImport(path)) {
      return importer->Import(path, out);
    }
  }
  return false;
}

}  // namespace ime::skin
