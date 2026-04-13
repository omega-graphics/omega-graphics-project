#include "assetc.h"

#include "omega-common/cli.h"
#include "omega-common/crypto.h"
#include "omega-common/json.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace {

using OmegaCommon::ArrayRef;
using OmegaCommon::DigestAlgorithm;
using OmegaCommon::Optional;
using OmegaCommon::Result;
using OmegaCommon::String;
using OmegaCommon::StrRef;
using OmegaCommon::Vector;

namespace assetc = OmegaCommon::assetc;
namespace fs = std::filesystem;

constexpr const char *ProgramName = "omega-assetc";
constexpr const char *AssetTypesConfigFileName = "AssetTypes.json";
constexpr const char *AssetTypesConfigEnvVar = "OMEGA_ASSET_TYPES_JSON";

struct CompilerOptions {
  bool help = false;
  bool compress = false;
  bool encrypt = false;
  bool sign = false;
  bool legacy = false;
  bool verbose = false;
  bool keyPassphrase = false;
  String outputFile;
  String appId;
  String keyFile;
  String manifestFile;
  String assetTypesFile;
  Vector<String> typeOverrides;
  Vector<String> stripPrefixes;
  Vector<String> inputs;
};

struct InputSpec {
  fs::path sourcePath;
  String declaredPath;
  Optional<assetc::AssetType> explicitType;
};

struct CompiledAsset {
  fs::path sourcePath;
  String declaredPath;
  String bundleName;
  assetc::AssetType type = assetc::AssetType::Raw;
  Vector<std::uint8_t> rawBytes;
  Vector<std::uint8_t> storedBytes;
  std::array<std::uint8_t, 32> entryHash {};
  std::uint32_t flags = static_cast<std::uint32_t>(assetc::AssetEntryFlags::None);
};

using TypeOverrideMap = std::unordered_map<String, assetc::AssetType>;

struct AssetTypeConfig {
  fs::path sourcePath;
  TypeOverrideMap extensionTypes;
};

String toLowerCopy(StrRef value) {
  String out(value.data(), value.size());
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

String trimCopy(const String &value) {
  size_t start = 0;
  while (start < value.size() &&
         std::isspace(static_cast<unsigned char>(value[start])) != 0) {
    ++start;
  }

  size_t end = value.size();
  while (end > start &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }

  return value.substr(start, end - start);
}

String unquoteJsonString(StrRef value) {
  if (value.size() >= 2 && value[0] == '"' && value[value.size() - 1] == '"') {
    return String(value.data() + 1, value.size() - 2);
  }
  return String(value.data(), value.size());
}

String normalizeLogicalPath(StrRef pathValue) {
  if (pathValue.data() == nullptr || pathValue.size() == 0) {
    return {};
  }

  auto normalized = fs::path(String(pathValue.data(), pathValue.size()))
                        .lexically_normal()
                        .generic_string();
  if (normalized == ".") {
    return {};
  }
  return normalized;
}

String trimTrailingSlash(String value) {
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

bool startsWithPathPrefix(StrRef pathValue, StrRef prefix) {
  if (prefix.size() == 0) {
    return false;
  }

  if (pathValue.size() < prefix.size()) {
    return false;
  }

  for (size_t i = 0; i < prefix.size(); ++i) {
    if (pathValue[i] != prefix[i]) {
      return false;
    }
  }

  return pathValue.size() == prefix.size() || pathValue[prefix.size()] == '/';
}

String normalizeExtension(StrRef extensionValue) {
  auto trimmed = trimCopy(unquoteJsonString(extensionValue));
  auto normalized = toLowerCopy(trimmed);
  while (!normalized.empty() && normalized.front() == '.') {
    normalized.erase(normalized.begin());
  }
  return normalized;
}

Result<assetc::AssetType, String> parseAssetType(StrRef typeName) {
  auto normalized = toLowerCopy(typeName);
  if (normalized == "raw") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Raw);
  }
  if (normalized == "image") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Image);
  }
  if (normalized == "font") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Font);
  }
  if (normalized == "shader") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Shader);
  }
  if (normalized == "text") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Text);
  }
  if (normalized == "audio") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Audio);
  }
  if (normalized == "binary") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Binary);
  }
  if (normalized == "model" || normalized == "mesh") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Model);
  }
  if (normalized == "material") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Material);
  }
  if (normalized == "scene" || normalized == "level" || normalized == "prefab") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Scene);
  }
  if (normalized == "animation" || normalized == "anim") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Animation);
  }
  if (normalized == "skeleton" || normalized == "rig") {
    return Result<assetc::AssetType, String>::ok(assetc::AssetType::Skeleton);
  }

  return Result<assetc::AssetType, String>::err("Unknown asset type: " +
                                                String(typeName.data(), typeName.size()));
}

String extensionForPath(const fs::path &path) {
  auto ext = toLowerCopy(path.extension().generic_string());
  if (!ext.empty() && ext.front() == '.') {
    ext.erase(ext.begin());
  }
  return ext;
}

Result<fs::path, String> resolveAssetTypesConfigPath(const CompilerOptions &options,
                                                     const fs::path &argv0) {
  if (!options.assetTypesFile.empty()) {
    return Result<fs::path, String>::ok(fs::path(options.assetTypesFile));
  }

  if (const char *envPath = std::getenv(AssetTypesConfigEnvVar)) {
    if (*envPath != '\0') {
      return Result<fs::path, String>::ok(fs::path(envPath));
    }
  }

  Vector<fs::path> candidates;
  auto executablePath = fs::absolute(argv0);
  if (executablePath.has_parent_path()) {
    candidates.push_back(executablePath.parent_path() / AssetTypesConfigFileName);
  }
  candidates.push_back(fs::current_path() / "common" / "assetc" / AssetTypesConfigFileName);

  for (const auto &candidate : candidates) {
    std::error_code ec;
    if (fs::exists(candidate, ec) && !ec) {
      return Result<fs::path, String>::ok(candidate);
    }
  }

  String searched;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (i != 0) {
      searched.append(", ");
    }
    searched.append(candidates[i].string());
  }

  return Result<fs::path, String>::err(
      "Unable to locate " + String(AssetTypesConfigFileName) + ". Looked in: " + searched);
}

Result<AssetTypeConfig, String> loadAssetTypeConfig(const CompilerOptions &options,
                                                    const fs::path &argv0) {
  auto resolvedPath = resolveAssetTypesConfigPath(options, argv0);
  if (resolvedPath.isErr()) {
    return Result<AssetTypeConfig, String>::err(resolvedPath.error());
  }

  std::ifstream in(resolvedPath.value());
  if (!in.is_open()) {
    return Result<AssetTypeConfig, String>::err(
        "Failed to open asset type config: " + resolvedPath.value().string());
  }

  auto json = OmegaCommon::JSON::parse(in);
  if (!json.isArray()) {
    return Result<AssetTypeConfig, String>::err(
        "Asset type config must be a top-level array.");
  }

  AssetTypeConfig config {};
  config.sourcePath = resolvedPath.value();

  auto rules = json.asVector();
  for (size_t i = 0; i < rules.size(); ++i) {
    auto &rule = const_cast<OmegaCommon::JSON &>(rules.begin()[i]);
    if (!rule.isArray()) {
      return Result<AssetTypeConfig, String>::err(
          "Asset type config rule " + std::to_string(i) + " must be an array.");
    }

    auto fields = rule.asVector();
    if (fields.size() < 2) {
      return Result<AssetTypeConfig, String>::err(
          "Asset type config rule " + std::to_string(i) +
          " must contain a type name and at least one extension.");
    }

    auto &typeNode = const_cast<OmegaCommon::JSON &>(fields.begin()[0]);
    if (!typeNode.isString()) {
      return Result<AssetTypeConfig, String>::err(
          "Asset type config rule " + std::to_string(i) +
          " must begin with a string asset type.");
    }

    auto parsedType = parseAssetType(unquoteJsonString(typeNode.asString()));
    if (parsedType.isErr()) {
      return Result<AssetTypeConfig, String>::err(
          "Asset type config rule " + std::to_string(i) + ": " + parsedType.error());
    }

    for (size_t j = 1; j < fields.size(); ++j) {
      auto &extensionNode = const_cast<OmegaCommon::JSON &>(fields.begin()[j]);
      if (!extensionNode.isString()) {
        return Result<AssetTypeConfig, String>::err(
            "Asset type config rule " + std::to_string(i) +
            " contains a non-string extension value.");
      }

      auto extension = normalizeExtension(extensionNode.asString());
      if (extension.empty()) {
        return Result<AssetTypeConfig, String>::err(
            "Asset type config rule " + std::to_string(i) +
            " contains an empty extension.");
      }

      config.extensionTypes[extension] = parsedType.value();
    }
  }

  if (config.extensionTypes.empty()) {
    return Result<AssetTypeConfig, String>::err(
        "Asset type config did not define any extensions.");
  }

  return Result<AssetTypeConfig, String>::ok(std::move(config));
}

assetc::AssetType inferAssetType(const fs::path &path, const AssetTypeConfig &config) {
  auto ext = extensionForPath(path);
  auto it = config.extensionTypes.find(ext);
  if (it != config.extensionTypes.end()) {
    return it->second;
  }

  return assetc::AssetType::Raw;
}

Result<std::array<std::uint8_t, 32>, String> sha256(Vector<std::uint8_t> &bytes) {
  auto hashResult = OmegaCommon::digest(DigestAlgorithm::SHA256,
                                        ArrayRef<std::uint8_t>(bytes));
  if (hashResult.isErr()) {
    return Result<std::array<std::uint8_t, 32>, String>::err(
        "SHA-256 failed: " + hashResult.error().message);
  }

  auto &digestBytes = hashResult.value().bytes;
  if (digestBytes.size() != 32) {
    return Result<std::array<std::uint8_t, 32>, String>::err(
        "SHA-256 returned an unexpected digest length.");
  }

  std::array<std::uint8_t, 32> out {};
  std::copy(digestBytes.begin(), digestBytes.end(), out.begin());
  return Result<std::array<std::uint8_t, 32>, String>::ok(std::move(out));
}

Result<TypeOverrideMap, String> parseTypeOverrides(const Vector<String> &rawOverrides) {
  TypeOverrideMap overrides;
  for (const auto &overrideSpec : rawOverrides) {
    auto eq = overrideSpec.find('=');
    if (eq == String::npos || eq == 0 || (eq + 1) >= overrideSpec.size()) {
      return Result<TypeOverrideMap, String>::err(
          "Invalid --type override: " + overrideSpec +
          ". Expected <name>=<type>.");
    }

    auto name = normalizeLogicalPath(
        StrRef(overrideSpec.data(), static_cast<StrRef::size_type>(eq)));
    auto parsedType = parseAssetType(
        StrRef(overrideSpec.data() + eq + 1,
               static_cast<StrRef::size_type>(overrideSpec.size() - eq - 1)));
    if (parsedType.isErr()) {
      return Result<TypeOverrideMap, String>::err(parsedType.error());
    }

    overrides[name] = parsedType.value();
  }

  return Result<TypeOverrideMap, String>::ok(std::move(overrides));
}

Vector<String> normalizeStripPrefixes(const Vector<String> &rawPrefixes) {
  Vector<String> normalized;
  normalized.reserve(rawPrefixes.size());
  for (const auto &prefix : rawPrefixes) {
    auto path = trimTrailingSlash(normalizeLogicalPath(prefix));
    if (!path.empty()) {
      normalized.push_back(std::move(path));
    }
  }

  std::sort(normalized.begin(), normalized.end(),
            [](const String &lhs, const String &rhs) {
              return lhs.size() > rhs.size();
            });
  return normalized;
}

String deriveBundleName(StrRef declaredPath, const Vector<String> &stripPrefixes) {
  String assetName = normalizeLogicalPath(declaredPath);
  for (const auto &prefix : stripPrefixes) {
    if (startsWithPathPrefix(assetName, prefix)) {
      assetName.erase(0, prefix.size());
      while (!assetName.empty() && assetName.front() == '/') {
        assetName.erase(assetName.begin());
      }
      break;
    }
  }
  return assetName;
}

Result<InputSpec, String> parseManifestLine(const String &line, const fs::path &manifestDir) {
  std::istringstream stream(line);
  String pathToken;
  stream >> pathToken;
  if (pathToken.empty()) {
    return Result<InputSpec, String>::err("Manifest entry is missing an asset path.");
  }

  InputSpec spec {};
  spec.declaredPath = normalizeLogicalPath(pathToken);
  if (spec.declaredPath.empty()) {
    return Result<InputSpec, String>::err("Manifest entry resolved to an empty asset path.");
  }

  spec.sourcePath = manifestDir / fs::path(pathToken);

  String token;
  while (stream >> token) {
    auto eq = token.find('=');
    if (eq == String::npos || eq == 0 || (eq + 1) >= token.size()) {
      return Result<InputSpec, String>::err("Invalid manifest attribute: " + token);
    }

    auto key = toLowerCopy(StrRef(token.data(), static_cast<StrRef::size_type>(eq)));
    auto value = String(token.data() + eq + 1, token.size() - eq - 1);
    if (key == "type") {
      auto parsedType = parseAssetType(value);
      if (parsedType.isErr()) {
        return Result<InputSpec, String>::err(parsedType.error());
      }
      spec.explicitType = parsedType.value();
      continue;
    }

    return Result<InputSpec, String>::err("Unsupported manifest attribute: " + key);
  }

  return Result<InputSpec, String>::ok(std::move(spec));
}

Result<Vector<InputSpec>, String> gatherInputs(const CompilerOptions &options) {
  Vector<InputSpec> inputs;

  if (!options.manifestFile.empty()) {
    auto manifestPath = fs::path(options.manifestFile);
    std::ifstream manifestIn(manifestPath);
    if (!manifestIn.is_open()) {
      return Result<Vector<InputSpec>, String>::err("Failed to open manifest file: " +
                                                    options.manifestFile);
    }

    auto manifestDir = manifestPath.has_parent_path() ? manifestPath.parent_path()
                                                      : fs::current_path();
    String line;
    size_t lineNumber = 0;
    while (std::getline(manifestIn, line)) {
      ++lineNumber;
      auto commentPos = line.find('#');
      if (commentPos != String::npos) {
        line.erase(commentPos);
      }

      auto trimmed = trimCopy(line);
      if (trimmed.empty()) {
        continue;
      }

      auto parsed = parseManifestLine(trimmed, manifestDir);
      if (parsed.isErr()) {
        return Result<Vector<InputSpec>, String>::err("Manifest " +
                                                      options.manifestFile + ":" +
                                                      std::to_string(lineNumber) + ": " +
                                                      parsed.error());
      }
      inputs.push_back(std::move(parsed.value()));
    }
  }

  for (const auto &input : options.inputs) {
    InputSpec spec {};
    spec.declaredPath = normalizeLogicalPath(input);
    spec.sourcePath = fs::path(input);
    inputs.push_back(std::move(spec));
  }

  if (inputs.empty()) {
    return Result<Vector<InputSpec>, String>::err(
        "No input files were provided. Use positional inputs or --manifest.");
  }

  return Result<Vector<InputSpec>, String>::ok(std::move(inputs));
}

Optional<assetc::AssetType> findOverrideType(const TypeOverrideMap &overrides,
                                             StrRef key0,
                                             StrRef key1) {
  auto it0 = overrides.find(String(key0.data(), key0.size()));
  if (it0 != overrides.end()) {
    return it0->second;
  }

  auto it1 = overrides.find(String(key1.data(), key1.size()));
  if (it1 != overrides.end()) {
    return it1->second;
  }

  return std::nullopt;
}

Result<CompiledAsset, String> compileAsset(const InputSpec &input,
                                           const TypeOverrideMap &typeOverrides,
                                           const Vector<String> &stripPrefixes,
                                           const AssetTypeConfig &assetTypeConfig) {
  CompiledAsset asset {};
  asset.sourcePath = input.sourcePath;
  asset.declaredPath = input.declaredPath;
  asset.bundleName = deriveBundleName(input.declaredPath, stripPrefixes);

  if (asset.bundleName.empty()) {
    return Result<CompiledAsset, String>::err("Asset path \"" + input.declaredPath +
                                              "\" became empty after strip-prefix processing.");
  }

  if (!fs::exists(asset.sourcePath) || !fs::is_regular_file(asset.sourcePath)) {
    return Result<CompiledAsset, String>::err("Input asset does not exist: " +
                                              asset.sourcePath.string());
  }

  std::ifstream in(asset.sourcePath, std::ios::binary | std::ios::ate);
  if (!in.is_open()) {
    return Result<CompiledAsset, String>::err("Failed to open asset file: " +
                                              asset.sourcePath.string());
  }

  auto size = in.tellg();
  if (size < 0) {
    return Result<CompiledAsset, String>::err("Failed to measure asset file: " +
                                              asset.sourcePath.string());
  }

  in.seekg(0, std::ios::beg);
  asset.rawBytes.resize(static_cast<size_t>(size));
  if (!asset.rawBytes.empty()) {
    in.read(reinterpret_cast<char *>(asset.rawBytes.data()), size);
  }

  if (in.bad()) {
    return Result<CompiledAsset, String>::err("Failed to read asset file: " +
                                              asset.sourcePath.string());
  }

  auto overrideType =
      findOverrideType(typeOverrides, input.declaredPath, asset.bundleName);
  if (input.explicitType.has_value()) {
    asset.type = *input.explicitType;
  } else if (overrideType.has_value()) {
    asset.type = *overrideType;
  } else {
    asset.type = inferAssetType(asset.sourcePath, assetTypeConfig);
  }

  auto hashResult = sha256(asset.rawBytes);
  if (hashResult.isErr()) {
    return Result<CompiledAsset, String>::err("While hashing \"" + input.declaredPath +
                                              "\": " + hashResult.error());
  }
  asset.entryHash = hashResult.value();
  asset.storedBytes = asset.rawBytes;

  return Result<CompiledAsset, String>::ok(std::move(asset));
}

template <typename T>
void appendStructBytes(Vector<std::uint8_t> &out, const T &value) {
  auto ptr = reinterpret_cast<const std::uint8_t *>(std::addressof(value));
  out.insert(out.end(), ptr, ptr + sizeof(T));
}

void appendBytes(Vector<std::uint8_t> &out, const Vector<std::uint8_t> &bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

Result<void *, String> writeLegacyBundle(const CompilerOptions &options,
                                         const Vector<CompiledAsset> &assets) {
  assetc::LegacyAssetsFileHeader header {};
  header.asset_count = static_cast<std::uint32_t>(assets.size());

  std::ofstream out(options.outputFile, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return Result<void *, String>::err("Failed to open output file: " + options.outputFile);
  }

  out.write(reinterpret_cast<const char *>(&header), sizeof(header));
  for (const auto &asset : assets) {
    assetc::LegacyAssetsFileEntry entry {};
    entry.string_size = static_cast<std::uint64_t>(asset.bundleName.size());
    entry.file_size = static_cast<std::uint64_t>(asset.rawBytes.size());

    out.write(reinterpret_cast<const char *>(&entry), sizeof(entry));
    if (!asset.bundleName.empty()) {
      out.write(asset.bundleName.data(),
                static_cast<std::streamsize>(asset.bundleName.size()));
    }
    if (!asset.rawBytes.empty()) {
      out.write(reinterpret_cast<const char *>(asset.rawBytes.data()),
                static_cast<std::streamsize>(asset.rawBytes.size()));
    }
  }

  if (!out.good()) {
    return Result<void *, String>::err("Failed while writing output file: " +
                                       options.outputFile);
  }

  return Result<void *, String>::ok(nullptr);
}

Result<void *, String> writeBundleV2(const CompilerOptions &options,
                                     const Vector<CompiledAsset> &assets) {
  Vector<assetc::AssetEntry> entries;
  entries.reserve(assets.size());

  Vector<std::uint8_t> stringTable;
  Vector<std::uint8_t> dataRegion;

  std::uint64_t currentOffset = 0;
  for (const auto &asset : assets) {
    assetc::AssetEntry entry {};
    entry.nameOffset = static_cast<std::uint32_t>(stringTable.size());
    entry.nameLength = static_cast<std::uint16_t>(asset.bundleName.size());
    entry.assetType = static_cast<std::uint16_t>(asset.type);
    entry.dataOffset = currentOffset;
    entry.rawSize = static_cast<std::uint64_t>(asset.rawBytes.size());
    entry.storedSize = static_cast<std::uint64_t>(asset.storedBytes.size());
    entry.flags = asset.flags;
    std::copy(asset.entryHash.begin(), asset.entryHash.end(), entry.entryHash);

    stringTable.insert(stringTable.end(), asset.bundleName.begin(), asset.bundleName.end());
    stringTable.push_back(0);

    appendBytes(dataRegion, asset.storedBytes);
    currentOffset += entry.storedSize;
    entries.push_back(entry);
  }

  assetc::BundleHeader header {};
  header.version = assetc::BundleVersion;
  header.entryCount = static_cast<std::uint32_t>(entries.size());
  header.stringTableSize = static_cast<std::uint32_t>(stringTable.size());
  header.dataRegionOffset =
      sizeof(assetc::BundleHeader) +
      (sizeof(assetc::AssetEntry) * static_cast<std::uint64_t>(entries.size())) +
      static_cast<std::uint64_t>(stringTable.size());
  header.dataRegionSize = static_cast<std::uint64_t>(dataRegion.size());
  header.flags = static_cast<std::uint16_t>(assetc::BundleFlags::None);
  if (options.sign) {
    header.flags |= static_cast<std::uint16_t>(assetc::BundleFlags::Signed);
  }

  if (options.sign) {
    Vector<std::uint8_t> hashInput;
    hashInput.reserve(entries.size() * sizeof(assetc::AssetEntry) +
                      stringTable.size() + dataRegion.size());

    for (const auto &entry : entries) {
      appendStructBytes(hashInput, entry);
    }
    appendBytes(hashInput, stringTable);
    appendBytes(hashInput, dataRegion);

    auto hashResult = sha256(hashInput);
    if (hashResult.isErr()) {
      return Result<void *, String>::err("Failed to compute bundle hash: " +
                                         hashResult.error());
    }

    std::copy(hashResult.value().begin(), hashResult.value().end(),
              header.bundleHash);
  }

  std::ofstream out(options.outputFile, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return Result<void *, String>::err("Failed to open output file: " + options.outputFile);
  }

  out.write(reinterpret_cast<const char *>(&header), sizeof(header));
  if (!entries.empty()) {
    out.write(reinterpret_cast<const char *>(entries.data()),
              static_cast<std::streamsize>(entries.size() * sizeof(assetc::AssetEntry)));
  }
  if (!stringTable.empty()) {
    out.write(reinterpret_cast<const char *>(stringTable.data()),
              static_cast<std::streamsize>(stringTable.size()));
  }
  if (!dataRegion.empty()) {
    out.write(reinterpret_cast<const char *>(dataRegion.data()),
              static_cast<std::streamsize>(dataRegion.size()));
  }

  if (!out.good()) {
    return Result<void *, String>::err("Failed while writing output file: " +
                                       options.outputFile);
  }

  return Result<void *, String>::ok(nullptr);
}

void printHelp(const OmegaCommon::Argv::Parser &parser) {
  parser.printHelp(std::cout);
  std::cout << std::endl;
  std::cout << "Notes:\n"
            << "  Asset type inference is loaded from " << AssetTypesConfigFileName
            << " at runtime.\n"
            << "  --compress and --encrypt are reserved for later phases and currently\n"
            << "  return an error if requested.\n";
}

void printVerboseAsset(const CompiledAsset &asset) {
  std::cout << "  " << asset.bundleName << "\n"
            << "    source: " << asset.sourcePath.string() << "\n"
            << "    type: " << OmegaCommon::assetTypeName(asset.type) << "\n"
            << "    raw size: " << asset.rawBytes.size() << " bytes\n"
            << "    stored size: " << asset.storedBytes.size() << " bytes\n";
}

Result<void *, String> validateOptions(const CompilerOptions &options) {
  if (options.outputFile.empty()) {
    return Result<void *, String>::err("Missing required option: --output.");
  }

  if (options.compress) {
    return Result<void *, String>::err(
        "Compression is planned for a later asset pipeline phase and is not implemented yet.");
  }

  if (options.encrypt || !options.keyFile.empty() || options.keyPassphrase) {
    return Result<void *, String>::err(
        "Encryption is planned for a later asset pipeline phase and is not implemented yet.");
  }

  if (options.legacy && options.sign) {
    return Result<void *, String>::err(
        "--sign cannot be combined with --legacy because the v1 format has no hash metadata.");
  }

  if (options.manifestFile.empty() && options.inputs.empty()) {
    return Result<void *, String>::err(
        "No input files were provided. Use positional inputs or --manifest.");
  }

  return Result<void *, String>::ok(nullptr);
}

} // namespace

int main(int argc, char *const argv[]) {
  CompilerOptions options {};

  OmegaCommon::Argv::Parser parser(ProgramName);
  parser.setDescription("Omega asset compiler");
  parser.setUsage("[options] <inputs...>");
  parser.addFlag(options.help, "help", "h", "Show this help text.");
  parser.addOption(options.outputFile, "output", "o", "file",
                   "Output bundle path (required).");
  parser.addOption(options.appId, "app-id", {}, "id",
                   "Application identifier reserved for signing/encryption phases.");
  parser.addOption(options.appId, "application-id", {}, "id",
                   "Backward-compatible alias for --app-id.");
  parser.addFlag(options.compress, "compress", {}, "Enable compression (not implemented yet).");
  parser.addFlag(options.encrypt, "encrypt", {}, "Enable encryption (not implemented yet).");
  parser.addOption(options.keyFile, "key-file", {}, "path",
                   "Encryption key file (not implemented yet).");
  parser.addFlag(options.keyPassphrase, "key-passphrase", {},
                 "Derive an encryption key from a passphrase (not implemented yet).");
  parser.addFlag(options.sign, "sign", {}, "Compute and embed the bundle hash.");
  parser.addMultiOption(options.typeOverrides, "type", {}, "name=type",
                        "Override the inferred asset type for a specific asset.");
  parser.addMultiOption(options.stripPrefixes, "strip-prefix", {}, "prefix",
                        "Strip a path prefix from logical asset names.");
  parser.addOption(options.manifestFile, "manifest", {}, "file",
                   "Read asset entries from a manifest file.");
  parser.addOption(options.assetTypesFile, "asset-types", {}, "file",
                   "Asset type JSON config path. Defaults to AssetTypes.json.");
  parser.addFlag(options.legacy, "legacy", {},
                 "Write the legacy v1 flat .pak format.");
  parser.addFlag(options.verbose, "verbose", "v",
                 "Print per-asset details while compiling.");
  parser.addPositional(options.inputs, "inputs", "Input asset files.", false);

  OmegaCommon::Argv::ParseError parseError;
  if (!parser.parse(argc, argv, &parseError)) {
    std::cerr << ProgramName << ": error: " << parseError.toString() << std::endl;
    std::cerr << "Use --help for usage." << std::endl;
    return 1;
  }

  if (options.help) {
    printHelp(parser);
    return 0;
  }

  auto validation = validateOptions(options);
  if (validation.isErr()) {
    std::cerr << ProgramName << ": error: " << validation.error() << std::endl;
    return 1;
  }

  auto typeOverrides = parseTypeOverrides(options.typeOverrides);
  if (typeOverrides.isErr()) {
    std::cerr << ProgramName << ": error: " << typeOverrides.error() << std::endl;
    return 1;
  }

  auto gatheredInputs = gatherInputs(options);
  if (gatheredInputs.isErr()) {
    std::cerr << ProgramName << ": error: " << gatheredInputs.error() << std::endl;
    return 1;
  }

  auto stripPrefixes = normalizeStripPrefixes(options.stripPrefixes);
  auto assetTypeConfig = loadAssetTypeConfig(options, argc > 0 ? argv[0] : ProgramName);
  if (assetTypeConfig.isErr()) {
    std::cerr << ProgramName << ": error: " << assetTypeConfig.error() << std::endl;
    return 1;
  }

  if (options.verbose) {
    std::cout << "Loaded asset type config: " << assetTypeConfig.value().sourcePath.string()
              << std::endl;
  }

  Vector<CompiledAsset> assets;
  assets.reserve(gatheredInputs.value().size());
  std::unordered_map<String, String> seenAssetNames;

  for (const auto &input : gatheredInputs.value()) {
    auto compiled = compileAsset(input, typeOverrides.value(), stripPrefixes,
                                 assetTypeConfig.value());
    if (compiled.isErr()) {
      std::cerr << ProgramName << ": error: " << compiled.error() << std::endl;
      return 1;
    }

    auto existing = seenAssetNames.find(compiled.value().bundleName);
    if (existing != seenAssetNames.end()) {
      std::cerr << ProgramName << ": error: Duplicate asset name \""
                << compiled.value().bundleName << "\" from \""
                << compiled.value().declaredPath << "\" and \"" << existing->second
                << "\"." << std::endl;
      return 1;
    }

    seenAssetNames[compiled.value().bundleName] = compiled.value().declaredPath;
    if (options.verbose) {
      printVerboseAsset(compiled.value());
    }
    assets.push_back(std::move(compiled.value()));
  }

  auto outputPath = fs::path(options.outputFile);
  if (outputPath.has_parent_path()) {
    std::error_code ec;
    fs::create_directories(outputPath.parent_path(), ec);
    if (ec) {
      std::cerr << ProgramName << ": error: Failed to create output directory: "
                << outputPath.parent_path().string() << std::endl;
      return 1;
    }
  }

  auto writeResult = options.legacy ? writeLegacyBundle(options, assets)
                                    : writeBundleV2(options, assets);
  if (writeResult.isErr()) {
    std::cerr << ProgramName << ": error: " << writeResult.error() << std::endl;
    return 1;
  }

  std::cout << ProgramName << ": wrote " << assets.size() << " asset"
            << (assets.size() == 1 ? "" : "s") << " to " << options.outputFile
            << (options.legacy ? " (legacy v1 format)" : " (bundle v2 format)")
            << std::endl;
  return 0;
}
