#include "omega-common/assets.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

using OmegaCommon::AssetBundle;
using OmegaCommon::AssetInfo;
using OmegaCommon::AssetType;
using OmegaCommon::FS::Path;
using OmegaCommon::String;
using OmegaCommon::Vector;

struct ExpectedAsset {
  const char *name;
  AssetType type;
  const char *contents;
};

constexpr const char *kAppConfig =
    "{\n"
    "  \"name\": \"DemoApp\",\n"
    "  \"version\": 1,\n"
    "  \"window\": {\n"
    "    \"width\": 1280,\n"
    "    \"height\": 720\n"
    "  }\n"
    "}\n";

constexpr const char *kBasicShader =
    "// Demo shader fixture used by omega-assetc tests.\n"
    "shader DemoBasic {\n"
    "  stage = \"fragment\"\n"
    "}\n";

constexpr const char *kHeroMaterial =
    "material HeroMaterial\n"
    "{\n"
    "  albedo = [0.9, 0.7, 0.2]\n"
    "  roughness = 0.35\n"
    "}\n";

constexpr const char *kIntroScene =
    "scene Intro\n"
    "{\n"
    "  spawn = \"PlayerStart\"\n"
    "  sky = \"ClearMorning\"\n"
    "}\n";

Vector<ExpectedAsset> expectedAssets() {
  return {
      {"Config/AppConfig.json", AssetType::Text, kAppConfig},
      {"Shaders/Basic.omegasl", AssetType::Shader, kBasicShader},
      {"Materials/Hero.asset", AssetType::Material, kHeroMaterial},
      {"Worlds/Intro.scene", AssetType::Scene, kIntroScene},
  };
}

bool expect(bool condition, const String &message) {
  if (!condition) {
    std::cerr << "omega-assetbundle-verifier: error: " << message << std::endl;
    return false;
  }
  return true;
}

bool verifyEntryOrder(const Vector<AssetInfo> &entries,
                      const Vector<ExpectedAsset> &expected) {
  if (!expect(entries.size() == expected.size(),
              "Unexpected entry count from entries().")) {
    return false;
  }

  for (size_t i = 0; i < expected.size(); ++i) {
    if (!expect(entries[i].name == expected[i].name,
                "Unexpected asset order at index " + std::to_string(i) + ".")) {
      return false;
    }
    if (!expect(entries[i].type == expected[i].type,
                "Unexpected asset type for " + entries[i].name + ".")) {
      return false;
    }
  }

  return true;
}

bool verifyBundle(const String &bundlePath) {
  auto openResult = AssetBundle::open(Path(bundlePath));
  if (openResult.isErr()) {
    std::cerr << "omega-assetbundle-verifier: error: " << openResult.error()
              << std::endl;
    return false;
  }

  auto bundle = std::move(openResult.value());
  auto expected = expectedAssets();

  if (!expect(bundle.entryCount() == expected.size(),
              "Unexpected bundle entry count.")) {
    return false;
  }

  if (!verifyEntryOrder(bundle.entries(), expected)) {
    return false;
  }

  if (!expect(bundle.contains("Config/AppConfig.json"),
              "Bundle should contain Config/AppConfig.json.") ||
      !expect(!bundle.contains("Missing/Asset.txt"),
              "Bundle unexpectedly contains Missing/Asset.txt.")) {
    return false;
  }

  for (const auto &asset : expected) {
    auto info = bundle.info(asset.name);
    if (!expect(info.has_value(), "Missing info() entry for " + String(asset.name) + ".")) {
      return false;
    }

    if (!expect(info->type == asset.type,
                "Unexpected asset type for " + String(asset.name) + ".")) {
      return false;
    }

    auto expectedSize = std::char_traits<char>::length(asset.contents);
    if (!expect(info->rawSize == expectedSize,
                "Unexpected asset size for " + String(asset.name) + ".")) {
      return false;
    }

    auto bytesResult = bundle.load(asset.name);
    if (bytesResult.isErr()) {
      std::cerr << "omega-assetbundle-verifier: error: " << bytesResult.error()
                << std::endl;
      return false;
    }

    auto loaded = String(bytesResult.value().begin(), bytesResult.value().end());
    if (!expect(loaded == asset.contents,
                "Asset payload mismatch for " + String(asset.name) + ".")) {
      return false;
    }

    auto streamResult = bundle.stream(asset.name);
    if (streamResult.isErr()) {
      // Encrypted/compressed entries must be rejected cleanly. The existing
      // signed+encrypted bundle exercises this branch.
      const auto &message = streamResult.error();
      if (!expect(message.find("encrypted") != String::npos ||
                      message.find("compressed") != String::npos,
                  "Unexpected stream() error for " + String(asset.name) + ": " + message)) {
        return false;
      }
    } else {
      auto &stream = *streamResult.value();
      String streamed((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
      if (!expect(!stream.bad(),
                  "stream() reported a bad state while reading " + String(asset.name) + ".")) {
        return false;
      }
      if (!expect(streamed == loaded,
                  "stream() bytes differ from load() bytes for " + String(asset.name) + ".")) {
        return false;
      }
    }
  }

  auto textResult = bundle.loadText("Config/AppConfig.json");
  if (textResult.isErr()) {
    std::cerr << "omega-assetbundle-verifier: error: " << textResult.error()
              << std::endl;
    return false;
  }

  if (!expect(textResult.value() == kAppConfig,
              "loadText returned unexpected JSON contents.")) {
    return false;
  }

  auto shaderText = bundle.loadText("Shaders/Basic.omegasl");
  if (!expect(shaderText.isErr(),
              "loadText should reject shader-tagged assets in v2 bundles.")) {
    return false;
  }

  auto missing = bundle.load("Missing/Asset.txt");
  return expect(missing.isErr(), "load() should fail for missing assets.");
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "Usage: omega-assetbundle-verifier <bundle-path>"
              << std::endl;
    return 2;
  }

  return verifyBundle(argv[1]) ? 0 : 1;
}
