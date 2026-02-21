#include "Gen.h"
#include "Targets.def"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace autom {

inline std::string generateObjectID(const autom::StrRef &type, const autom::StrRef &otherData) {
    SHA256Hash shaHash;
    shaHash.addData((void *)type.data(), type.size());
    shaHash.addData((void *)otherData.data(), otherData.size());
    std::string out;
    shaHash.getResultAsHex(out);
    return out.substr(0, 24);
}

class XcodeGen final : public Gen {
    struct ObjectEntry {
        std::string id;
        std::string comment;
        std::string isa;
        std::vector<std::string> fields;
    };

    struct TargetEntry {
        std::shared_ptr<Target> target;
        std::string id;
        std::string configListId;
        std::string debugConfigId;
        std::string releaseConfigId;
        std::string name;
        bool native = false;
        bool script = false;
        std::string productRefId;
        std::string productPath;
        std::string productType;
        std::string productFileType;
        std::string sourcesPhaseId;
        std::string frameworksPhaseId;
        std::string shellScriptPhaseId;
        std::vector<std::string> phaseRefs;
        std::vector<std::string> dependencyRefs;
        std::vector<std::string> sourceBuildFileRefs;
        std::vector<std::string> frameworkBuildFileRefs;
    };

    OutputTargetOpts &outputOpts;
    GenXcodeOpts &genOpts;

    std::ofstream pbxprojOut;
    std::filesystem::path projectBundlePath;

    ToolchainDefaults *toolchainDefaults = nullptr;
    std::vector<std::shared_ptr<Target>> pendingTargets;

    std::vector<ObjectEntry> pbxBuildFiles;
    std::vector<ObjectEntry> pbxContainerItemProxies;
    std::vector<ObjectEntry> pbxFileReferences;
    std::vector<ObjectEntry> pbxFrameworkBuildPhases;
    std::vector<ObjectEntry> pbxGroupObjects;
    std::vector<ObjectEntry> pbxNativeTargets;
    std::vector<ObjectEntry> pbxAggregateTargets;
    std::vector<ObjectEntry> pbxProjectObjects;
    std::vector<ObjectEntry> pbxShellScriptBuildPhases;
    std::vector<ObjectEntry> pbxSourcesBuildPhases;
    std::vector<ObjectEntry> pbxTargetDependencies;
    std::vector<ObjectEntry> xcBuildConfigurations;
    std::vector<ObjectEntry> xcConfigurationLists;

    std::unordered_map<std::string, std::string> sourceFileRefByPath;
    std::unordered_map<std::string, std::string> frameworkFileRefByPath;
    std::unordered_map<std::string, std::string> productFileRefByPath;

    std::vector<std::string> sourceGroupChildren;
    std::vector<std::string> frameworkGroupChildren;
    std::vector<std::string> productsGroupChildren;

    std::unordered_map<const Target *, std::size_t> targetIndexByPtr;

    std::string rootProjectId;
    std::string mainGroupId;
    std::string productsGroupId;
    std::string sourcesGroupId;
    std::string frameworksGroupId;
    std::string projectConfigListId;
    std::string projectDebugConfigId;
    std::string projectReleaseConfigId;

    unsigned idCounter = 0;

    static std::string toPosixPath(std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        return path;
    }

    static std::string escapeString(const std::string &value) {
        std::string escaped;
        escaped.reserve(value.size() + 8);
        for (char c : value) {
            if (c == '\\' || c == '"') {
                escaped.push_back('\\');
            }
            escaped.push_back(c);
        }
        return escaped;
    }

    static std::string quoted(const std::string &value) {
        return "\"" + escapeString(value) + "\"";
    }

    static std::string refWithComment(const std::string &id, const std::string &comment) {
        return id + " /* " + comment + " */";
    }

    static std::string shellQuote(const std::string &value) {
        std::string out = "'";
        for (char c : value) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out.push_back(c);
            }
        }
        out.push_back('\'');
        return out;
    }

    static std::string formatArrayLiteral(const std::vector<std::string> &values, int indentLevel = 4) {
        std::ostringstream out;
        out << "(\n";
        for (const auto &value : values) {
            out << std::string(static_cast<std::size_t>(indentLevel), '\t') << value << ",\n";
        }
        out << std::string(static_cast<std::size_t>(indentLevel - 1), '\t') << ")";
        return out.str();
    }

    std::string makeID(const std::string &type, const std::string &otherData) {
        auto serial = idCounter++;
        auto h = std::hash<std::string>();
        unsigned h1 = static_cast<unsigned>(h(type));
        unsigned h2 = static_cast<unsigned>(h(otherData));
        unsigned h3 = static_cast<unsigned>(h(type + ":" + otherData + ":" + std::to_string(serial)));
        std::ostringstream out;
        out << std::uppercase << std::hex << std::setfill('0')
            << std::setw(8) << h1
            << std::setw(8) << h2
            << std::setw(8) << h3;
        return out.str();
    }

    std::string sdkRootForOutput() const {
        switch (outputOpts.platform) {
            case TargetPlatform::iOS:
                return "iphoneos";
            case TargetPlatform::macOS:
                return "macosx";
            default:
                return "macosx";
        }
    }

    static std::string fileTypeForSource(const std::string &path) {
        auto ext = std::filesystem::path(path).extension().string();
        if (ext == ".c") return "sourcecode.c.c";
        if (ext == ".cc" || ext == ".cpp" || ext == ".cxx") return "sourcecode.cpp.cpp";
        if (ext == ".m") return "sourcecode.c.objc";
        if (ext == ".mm") return "sourcecode.cpp.objcpp";
        if (ext == ".swift") return "sourcecode.swift";
        return "text";
    }

    std::string fileTypeForProduct(const CompiledTarget *target) const {
        if (target->type == EXECUTABLE) return "compiled.mach-o.executable";
        if (target->type == STATIC_LIBRARY) return "archive.ar";
        return "compiled.mach-o.dylib";
    }

    std::string productTypeForTarget(const CompiledTarget *target) const {
        if (target->type == EXECUTABLE) return "com.apple.product-type.tool";
        if (target->type == STATIC_LIBRARY) return "com.apple.product-type.library.static";
        return "com.apple.product-type.library.dynamic";
    }

    std::string targetOutputName(const CompiledTarget *target) const {
        std::string out = target->name->value().data();
        if (!target->output_ext->empty()) {
            out += "." + std::string(target->output_ext->value());
        }
        return out;
    }

    std::string targetProductPath(const CompiledTarget *target) const {
        auto name = targetOutputName(target);
        if (target->output_dir->empty()) {
            return name;
        }
        return toPosixPath((std::filesystem::path(target->output_dir->value().data()) / name).string());
    }

    std::string projectRelativePathExpr(const std::string &path) const {
        auto normalized = toPosixPath(path);
        auto p = std::filesystem::path(normalized);
        if (p.is_absolute()) {
            return normalized;
        }
        return std::string("$(PROJECT_DIR)/") + normalized;
    }

    std::string getOrCreateSourceFileRef(const std::string &sourcePath) {
        auto normalized = toPosixPath(sourcePath);
        auto existing = sourceFileRefByPath.find(normalized);
        if (existing != sourceFileRefByPath.end()) {
            return existing->second;
        }

        const auto pathObj = std::filesystem::path(normalized);
        auto id = makeID("PBXFileReference", normalized);
        ObjectEntry entry {};
        entry.id = id;
        entry.comment = pathObj.filename().string();
        entry.isa = "PBXFileReference";
        entry.fields = {
            "lastKnownFileType = " + fileTypeForSource(normalized) + ";",
            "name = " + quoted(pathObj.filename().string()) + ";",
            "path = " + quoted(normalized) + ";",
            "sourceTree = \"<group>\";"
        };
        pbxFileReferences.push_back(std::move(entry));
        sourceGroupChildren.push_back(refWithComment(id, pathObj.filename().string()));
        sourceFileRefByPath.insert({normalized, id});
        return id;
    }

    std::string getOrCreateFrameworkRef(const std::string &frameworkNameOrPath) {
        auto raw = toPosixPath(frameworkNameOrPath);
        std::string name;
        std::string path;
        std::string sourceTree;

        if (raw.find('/') != std::string::npos) {
            path = raw;
            name = std::filesystem::path(raw).filename().string();
            sourceTree = std::filesystem::path(raw).is_absolute() ? "\"<absolute>\"" : "\"<group>\"";
        } else {
            name = raw;
            if (name.find(".framework") == std::string::npos) {
                name += ".framework";
            }
            path = "System/Library/Frameworks/" + name;
            sourceTree = "SDKROOT";
        }

        auto key = sourceTree + ":" + path;
        auto existing = frameworkFileRefByPath.find(key);
        if (existing != frameworkFileRefByPath.end()) {
            return existing->second;
        }

        auto id = makeID("PBXFileReference", key);
        ObjectEntry entry {};
        entry.id = id;
        entry.comment = name;
        entry.isa = "PBXFileReference";
        entry.fields = {
            "lastKnownFileType = wrapper.framework;",
            "name = " + quoted(name) + ";",
            "path = " + quoted(path) + ";",
            "sourceTree = " + sourceTree + ";"
        };
        pbxFileReferences.push_back(std::move(entry));
        frameworkGroupChildren.push_back(refWithComment(id, name));
        frameworkFileRefByPath.insert({key, id});
        return id;
    }

    std::string getOrCreateProductRef(const std::string &productPath, const std::string &fileType) {
        auto normalized = toPosixPath(productPath);
        auto existing = productFileRefByPath.find(normalized);
        if (existing != productFileRefByPath.end()) {
            return existing->second;
        }

        auto name = std::filesystem::path(normalized).filename().string();
        auto id = makeID("PBXFileReference", "product:" + normalized);
        ObjectEntry entry {};
        entry.id = id;
        entry.comment = name;
        entry.isa = "PBXFileReference";
        entry.fields = {
            "explicitFileType = " + fileType + ";",
            "includeInIndex = 0;",
            "name = " + quoted(name) + ";",
            "path = " + quoted(normalized) + ";",
            "sourceTree = BUILT_PRODUCTS_DIR;"
        };
        pbxFileReferences.push_back(std::move(entry));
        productsGroupChildren.push_back(refWithComment(id, name));
        productFileRefByPath.insert({normalized, id});
        return id;
    }

    std::string addBuildFileRef(const std::string &name, const std::string &fileRefId) {
        auto id = makeID("PBXBuildFile", name + ":" + fileRefId);
        ObjectEntry entry {};
        entry.id = id;
        entry.comment = name + " in Build";
        entry.isa = "PBXBuildFile";
        entry.fields = {
            "fileRef = " + refWithComment(fileRefId, name) + ";"
        };
        pbxBuildFiles.push_back(std::move(entry));
        return id;
    }

    void addConfigEntries(TargetEntry &entry) {
        auto debug = makeID("XCBuildConfiguration", entry.name + ":Debug");
        auto release = makeID("XCBuildConfiguration", entry.name + ":Release");
        auto list = makeID("XCConfigurationList", entry.name + ":ConfigList");
        entry.debugConfigId = debug;
        entry.releaseConfigId = release;
        entry.configListId = list;
    }

    std::vector<std::string> mergeFlags(eval::Array *lhs, eval::Array *rhs) const {
        std::vector<std::string> out;
        if (lhs != nullptr) {
            auto vals = lhs->toStringVector();
            out.insert(out.end(), vals.begin(), vals.end());
        }
        if (rhs != nullptr) {
            auto vals = rhs->toStringVector();
            out.insert(out.end(), vals.begin(), vals.end());
        }
        return out;
    }

    std::string formatSettingsArrayValues(const std::vector<std::string> &values) const {
        std::vector<std::string> quotedValues;
        quotedValues.reserve(values.size());
        for (const auto &value : values) {
            quotedValues.push_back(quoted(toPosixPath(value)));
        }
        return formatArrayLiteral(quotedValues, 5);
    }

    std::string buildShellScript(const ScriptTarget *target) const {
        std::ostringstream script;
        script << "python3 " << shellQuote(projectRelativePathExpr(target->script->value()));
        auto args = target->args->toStringVector();
        for (const auto &arg : args) {
            script << " " << shellQuote(arg);
        }
        return script.str();
    }

    void resetState() {
        pendingTargets.clear();
        pbxBuildFiles.clear();
        pbxContainerItemProxies.clear();
        pbxFileReferences.clear();
        pbxFrameworkBuildPhases.clear();
        pbxGroupObjects.clear();
        pbxNativeTargets.clear();
        pbxAggregateTargets.clear();
        pbxProjectObjects.clear();
        pbxShellScriptBuildPhases.clear();
        pbxSourcesBuildPhases.clear();
        pbxTargetDependencies.clear();
        xcBuildConfigurations.clear();
        xcConfigurationLists.clear();
        sourceFileRefByPath.clear();
        frameworkFileRefByPath.clear();
        productFileRefByPath.clear();
        sourceGroupChildren.clear();
        frameworkGroupChildren.clear();
        productsGroupChildren.clear();
        targetIndexByPtr.clear();
        idCounter = 0;
    }

    void writeSection(const std::string &name, const std::vector<ObjectEntry> &entries) {
        if (entries.empty()) {
            return;
        }

        pbxprojOut << "\n\t\t/* Begin " << name << " section */\n";
        for (const auto &entry : entries) {
            pbxprojOut << "\t\t" << entry.id << " /* " << entry.comment << " */ = {\n";
            pbxprojOut << "\t\t\tisa = " << entry.isa << ";\n";
            for (const auto &field : entry.fields) {
                pbxprojOut << "\t\t\t" << field << '\n';
            }
            pbxprojOut << "\t\t};\n";
        }
        pbxprojOut << "\t\t/* End " << name << " section */\n";
    }

    void emitPBXProject(const std::vector<TargetEntry> &targets) {
        std::vector<std::string> targetRefs;
        for (const auto &target : targets) {
            targetRefs.push_back(refWithComment(target.id, target.name));
        }

        std::ostringstream projectAttributes;
        projectAttributes << "attributes = {\n";
        projectAttributes << "\t\t\t\tLastUpgradeCheck = 1300;\n";
        projectAttributes << "\t\t\t\tBuildIndependentTargetsInParallel = " << (genOpts.newBuildSystem ? "YES" : "NO") << ";\n";
        projectAttributes << "\t\t\t};";

        ObjectEntry project {};
        project.id = rootProjectId;
        project.comment = "Project object";
        project.isa = "PBXProject";
        project.fields = {
            projectAttributes.str(),
            "buildConfigurationList = " + refWithComment(projectConfigListId, "Build configuration list for PBXProject \"" + std::string(context->projectDesc.name) + "\"") + ";",
            "compatibilityVersion = \"Xcode 3.2\";",
            "developmentRegion = en;",
            "hasScannedForEncodings = 0;",
            "knownRegions = (\n\t\t\t\ten,\n\t\t\t\tBase,\n\t\t\t);",
            "mainGroup = " + refWithComment(mainGroupId, "Main Group") + ";",
            "productRefGroup = " + refWithComment(productsGroupId, "Products") + ";",
            "projectDirPath = \"\";",
            "projectRoot = \"\";",
            "targets = " + formatArrayLiteral(targetRefs, 4) + ";"
        };
        pbxProjectObjects.push_back(std::move(project));
    }

    void emitProjectConfiguration() {
        auto sdkRoot = sdkRootForOutput();

        ObjectEntry debug {};
        debug.id = projectDebugConfigId;
        debug.comment = "Debug";
        debug.isa = "XCBuildConfiguration";
        debug.fields = {
            "buildSettings = {\n\t\t\t\tALWAYS_SEARCH_USER_PATHS = NO;\n\t\t\t\tCLANG_CXX_LANGUAGE_STANDARD = \"gnu++17\";\n\t\t\t\tSDKROOT = " + sdkRoot + ";\n\t\t\t};",
            "name = Debug;"
        };
        xcBuildConfigurations.push_back(std::move(debug));

        ObjectEntry release {};
        release.id = projectReleaseConfigId;
        release.comment = "Release";
        release.isa = "XCBuildConfiguration";
        release.fields = {
            "buildSettings = {\n\t\t\t\tALWAYS_SEARCH_USER_PATHS = NO;\n\t\t\t\tCLANG_CXX_LANGUAGE_STANDARD = \"gnu++17\";\n\t\t\t\tSDKROOT = " + sdkRoot + ";\n\t\t\t};",
            "name = Release;"
        };
        xcBuildConfigurations.push_back(std::move(release));

        ObjectEntry configList {};
        configList.id = projectConfigListId;
        configList.comment = "Build configuration list for PBXProject \"" + std::string(context->projectDesc.name) + "\"";
        configList.isa = "XCConfigurationList";
        configList.fields = {
            "buildConfigurations = " + formatArrayLiteral({
                refWithComment(projectDebugConfigId, "Debug"),
                refWithComment(projectReleaseConfigId, "Release")
            }, 4) + ";",
            "defaultConfigurationIsVisible = 0;",
            "defaultConfigurationName = Release;"
        };
        xcConfigurationLists.push_back(std::move(configList));
    }

    void emitGroups() {
        ObjectEntry mainGroup {};
        mainGroup.id = mainGroupId;
        mainGroup.comment = "Main Group";
        mainGroup.isa = "PBXGroup";
        mainGroup.fields = {
            "children = " + formatArrayLiteral({
                refWithComment(sourcesGroupId, "Sources"),
                refWithComment(frameworksGroupId, "Frameworks"),
                refWithComment(productsGroupId, "Products")
            }, 4) + ";",
            "sourceTree = \"<group>\";"
        };
        pbxGroupObjects.push_back(std::move(mainGroup));

        ObjectEntry sourcesGroup {};
        sourcesGroup.id = sourcesGroupId;
        sourcesGroup.comment = "Sources";
        sourcesGroup.isa = "PBXGroup";
        sourcesGroup.fields = {
            "children = " + formatArrayLiteral(sourceGroupChildren, 4) + ";",
            "name = Sources;",
            "sourceTree = \"<group>\";"
        };
        pbxGroupObjects.push_back(std::move(sourcesGroup));

        ObjectEntry frameworksGroup {};
        frameworksGroup.id = frameworksGroupId;
        frameworksGroup.comment = "Frameworks";
        frameworksGroup.isa = "PBXGroup";
        frameworksGroup.fields = {
            "children = " + formatArrayLiteral(frameworkGroupChildren, 4) + ";",
            "name = Frameworks;",
            "sourceTree = \"<group>\";"
        };
        pbxGroupObjects.push_back(std::move(frameworksGroup));

        ObjectEntry productsGroup {};
        productsGroup.id = productsGroupId;
        productsGroup.comment = "Products";
        productsGroup.isa = "PBXGroup";
        productsGroup.fields = {
            "children = " + formatArrayLiteral(productsGroupChildren, 4) + ";",
            "name = Products;",
            "sourceTree = \"<group>\";"
        };
        pbxGroupObjects.push_back(std::move(productsGroup));
    }

    void emitTargetConfig(const TargetEntry &entry, const CompiledTarget *compiled) {
        std::vector<std::string> includeDirs;
        if (compiled != nullptr) {
            includeDirs = compiled->include_dirs->toStringVector();
        }
        std::vector<std::string> libraryDirs;
        std::vector<std::string> ldFlags;
        std::vector<std::string> frameworkDirs;
        std::vector<std::string> cFlags;
        std::vector<std::string> cxxFlags;

        if (compiled != nullptr) {
            libraryDirs = compiled->lib_dirs->toStringVector();
            ldFlags = compiled->ldflags->toStringVector();
            cFlags = mergeFlags(toolchainDefaults ? toolchainDefaults->c_flags : nullptr, compiled->cflags);
            cxxFlags = mergeFlags(toolchainDefaults ? toolchainDefaults->cxx_flags : nullptr, compiled->cflags);
#ifdef __APPLE__
            frameworkDirs = compiled->framework_dirs->toStringVector();
#endif
        }

        auto buildDir = std::string("$(BUILT_PRODUCTS_DIR)");
        if (compiled != nullptr && !compiled->output_dir->empty()) {
            buildDir = projectRelativePathExpr(compiled->output_dir->value());
        }

        auto productName = entry.name;
        auto productExt = compiled != nullptr ? std::string(compiled->output_ext->value()) : std::string();

        std::ostringstream debugSettings;
        debugSettings << "buildSettings = {\n";
        debugSettings << "\t\t\t\tPRODUCT_NAME = " << quoted(productName) << ";\n";
        if (compiled != nullptr) {
            debugSettings << "\t\t\t\tCONFIGURATION_BUILD_DIR = " << quoted(buildDir) << ";\n";
            if (!productExt.empty()) {
                debugSettings << "\t\t\t\tEXECUTABLE_EXTENSION = " << quoted(productExt) << ";\n";
            }
            if (compiled->type == STATIC_LIBRARY) {
                debugSettings << "\t\t\t\tMACH_O_TYPE = staticlib;\n";
            } else if (compiled->type == SHARED_LIBRARY) {
                debugSettings << "\t\t\t\tMACH_O_TYPE = mh_dylib;\n";
            }
            if (!includeDirs.empty()) {
                std::vector<std::string> vals;
                vals.push_back(quoted("$(inherited)"));
                for (const auto &dir : includeDirs) vals.push_back(quoted(toPosixPath(dir)));
                debugSettings << "\t\t\t\tHEADER_SEARCH_PATHS = " << formatArrayLiteral(vals, 5) << ";\n";
            }
            if (!libraryDirs.empty()) {
                std::vector<std::string> vals;
                vals.push_back(quoted("$(inherited)"));
                for (const auto &dir : libraryDirs) vals.push_back(quoted(toPosixPath(dir)));
                debugSettings << "\t\t\t\tLIBRARY_SEARCH_PATHS = " << formatArrayLiteral(vals, 5) << ";\n";
            }
            if (!frameworkDirs.empty()) {
                std::vector<std::string> vals;
                vals.push_back(quoted("$(inherited)"));
                for (const auto &dir : frameworkDirs) vals.push_back(quoted(toPosixPath(dir)));
                debugSettings << "\t\t\t\tFRAMEWORK_SEARCH_PATHS = " << formatArrayLiteral(vals, 5) << ";\n";
            }
            if (!cFlags.empty()) {
                std::vector<std::string> vals;
                vals.push_back(quoted("$(inherited)"));
                for (const auto &flag : cFlags) vals.push_back(quoted(flag));
                debugSettings << "\t\t\t\tOTHER_CFLAGS = " << formatArrayLiteral(vals, 5) << ";\n";
            }
            if (!cxxFlags.empty()) {
                std::vector<std::string> vals;
                vals.push_back(quoted("$(inherited)"));
                for (const auto &flag : cxxFlags) vals.push_back(quoted(flag));
                debugSettings << "\t\t\t\tOTHER_CPLUSPLUSFLAGS = " << formatArrayLiteral(vals, 5) << ";\n";
            }
            if (!ldFlags.empty()) {
                std::vector<std::string> vals;
                vals.push_back(quoted("$(inherited)"));
                for (const auto &flag : ldFlags) vals.push_back(quoted(flag));
                debugSettings << "\t\t\t\tOTHER_LDFLAGS = " << formatArrayLiteral(vals, 5) << ";\n";
            }
        }
        debugSettings << "\t\t\t};";

        ObjectEntry debug {};
        debug.id = entry.debugConfigId;
        debug.comment = "Debug";
        debug.isa = "XCBuildConfiguration";
        debug.fields = {
            debugSettings.str(),
            "name = Debug;"
        };
        xcBuildConfigurations.push_back(std::move(debug));

        std::ostringstream releaseSettings;
        releaseSettings << "buildSettings = {\n";
        releaseSettings << "\t\t\t\tPRODUCT_NAME = " << quoted(productName) << ";\n";
        if (compiled != nullptr) {
            releaseSettings << "\t\t\t\tCONFIGURATION_BUILD_DIR = " << quoted(buildDir) << ";\n";
            if (!productExt.empty()) {
                releaseSettings << "\t\t\t\tEXECUTABLE_EXTENSION = " << quoted(productExt) << ";\n";
            }
            if (compiled->type == STATIC_LIBRARY) {
                releaseSettings << "\t\t\t\tMACH_O_TYPE = staticlib;\n";
            } else if (compiled->type == SHARED_LIBRARY) {
                releaseSettings << "\t\t\t\tMACH_O_TYPE = mh_dylib;\n";
            }
        }
        releaseSettings << "\t\t\t};";

        ObjectEntry release {};
        release.id = entry.releaseConfigId;
        release.comment = "Release";
        release.isa = "XCBuildConfiguration";
        release.fields = {
            releaseSettings.str(),
            "name = Release;"
        };
        xcBuildConfigurations.push_back(std::move(release));

        ObjectEntry list {};
        list.id = entry.configListId;
        list.comment = "Build configuration list for " + std::string(entry.native ? "PBXNativeTarget" : "PBXAggregateTarget") + " \"" + entry.name + "\"";
        list.isa = "XCConfigurationList";
        list.fields = {
            "buildConfigurations = " + formatArrayLiteral({
                refWithComment(entry.debugConfigId, "Debug"),
                refWithComment(entry.releaseConfigId, "Release")
            }, 4) + ";",
            "defaultConfigurationIsVisible = 0;",
            "defaultConfigurationName = Release;"
        };
        xcConfigurationLists.push_back(std::move(list));
    }

public:
    explicit XcodeGen(OutputTargetOpts &outputOpts, GenXcodeOpts &genOpts)
        : outputOpts(outputOpts), genOpts(genOpts) {
    }

    void configGenContext() override {
        resetState();
        auto projectDir = std::string(context->projectDesc.name) + ".xcodeproj";
        projectBundlePath = std::filesystem::path(context->outputDir.data()) / projectDir;
        std::filesystem::create_directories(projectBundlePath);
        pbxprojOut.open(projectBundlePath / "project.pbxproj", std::ios::out);
    }

    void consumeToolchainDefaults(ToolchainDefaults &conf) override {
        toolchainDefaults = &conf;
    }

    bool supportsCustomToolchainRules() override {
        return false;
    }

    void genToolchainRules(std::shared_ptr<Toolchain> &) override {
    }

    void consumeTarget(std::shared_ptr<Target> &target) override {
        pendingTargets.push_back(target);
    }

    void finish() override {
        if (!pbxprojOut.is_open()) {
            return;
        }

        rootProjectId = makeID("PBXProject", context->projectDesc.name);
        mainGroupId = makeID("PBXGroup", "MainGroup");
        productsGroupId = makeID("PBXGroup", "Products");
        sourcesGroupId = makeID("PBXGroup", "Sources");
        frameworksGroupId = makeID("PBXGroup", "Frameworks");
        projectConfigListId = makeID("XCConfigurationList", "ProjectConfigList");
        projectDebugConfigId = makeID("XCBuildConfiguration", "ProjectDebug");
        projectReleaseConfigId = makeID("XCBuildConfiguration", "ProjectRelease");

        std::vector<TargetEntry> targets;
        targets.reserve(pendingTargets.size());
        for (const auto &target : pendingTargets) {
            if (IS_COMPILED_TARGET_TYPE(target->type) && target->type != SOURCE_GROUP) {
                auto compiled = std::dynamic_pointer_cast<CompiledTarget>(target);
                TargetEntry entry {};
                entry.target = target;
                entry.native = true;
                entry.name = compiled->name->value().data();
                entry.id = makeID("PBXNativeTarget", entry.name);
                entry.sourcesPhaseId = makeID("PBXSourcesBuildPhase", entry.name + ":Sources");
                entry.frameworksPhaseId = makeID("PBXFrameworksBuildPhase", entry.name + ":Frameworks");
                entry.phaseRefs = {
                    refWithComment(entry.sourcesPhaseId, "Sources"),
                    refWithComment(entry.frameworksPhaseId, "Frameworks")
                };
                entry.productType = productTypeForTarget(compiled.get());
                entry.productPath = targetProductPath(compiled.get());
                entry.productFileType = fileTypeForProduct(compiled.get());
                entry.productRefId = getOrCreateProductRef(entry.productPath, entry.productFileType);
                addConfigEntries(entry);
                targets.push_back(std::move(entry));
            } else if (target->type == GROUP_TARGET || target->type == SCRIPT_TARGET) {
                TargetEntry entry {};
                entry.target = target;
                entry.native = false;
                entry.script = target->type == SCRIPT_TARGET;
                entry.name = target->name->value().data();
                entry.id = makeID("PBXAggregateTarget", entry.name);
                if (entry.script) {
                    entry.shellScriptPhaseId = makeID("PBXShellScriptBuildPhase", entry.name + ":Script");
                    entry.phaseRefs.push_back(refWithComment(entry.shellScriptPhaseId, "ShellScript"));
                }
                addConfigEntries(entry);
                targets.push_back(std::move(entry));
            }
        }

        for (std::size_t i = 0; i < targets.size(); ++i) {
            targetIndexByPtr.insert({targets[i].target.get(), i});
        }

        for (auto &entry : targets) {
            if (!entry.native) {
                continue;
            }

            auto compiled = std::dynamic_pointer_cast<CompiledTarget>(entry.target);
            std::vector<std::string> sources;
            for (const auto &sourcePair : compiled->source_object_map) {
                sources.push_back(toPosixPath(sourcePair.first));
            }

            for (const auto &dep : entry.target->resolvedDeps) {
                if (dep->type == SOURCE_GROUP) {
                    auto sourceGroup = std::dynamic_pointer_cast<CompiledTarget>(dep);
                    for (const auto &sourcePair : sourceGroup->source_object_map) {
                        sources.push_back(toPosixPath(sourcePair.first));
                    }
                }
            }

            std::sort(sources.begin(), sources.end());
            sources.erase(std::unique(sources.begin(), sources.end()), sources.end());

            for (const auto &source : sources) {
                auto fileRefId = getOrCreateSourceFileRef(source);
                auto buildFileId = addBuildFileRef(std::filesystem::path(source).filename().string(), fileRefId);
                entry.sourceBuildFileRefs.push_back(refWithComment(buildFileId, std::filesystem::path(source).filename().string() + " in Sources"));
            }

            auto frameworks = compiled->frameworks->toStringVector();
            for (auto framework : frameworks) {
                if (framework.find(".framework") == std::string::npos && framework.find('/') == std::string::npos) {
                    framework += ".framework";
                }
                auto fileRefId = getOrCreateFrameworkRef(framework);
                auto buildFileId = addBuildFileRef(std::filesystem::path(framework).filename().string(), fileRefId);
                entry.frameworkBuildFileRefs.push_back(refWithComment(buildFileId, std::filesystem::path(framework).filename().string() + " in Frameworks"));
            }
        }

        for (auto &entry : targets) {
            for (const auto &dep : entry.target->resolvedDeps) {
                auto found = targetIndexByPtr.find(dep.get());
                if (found == targetIndexByPtr.end()) {
                    continue;
                }

                auto &depEntry = targets[found->second];
                if (dep->type == SOURCE_GROUP) {
                    continue;
                }

                auto proxyId = makeID("PBXContainerItemProxy", entry.name + "->" + depEntry.name);
                ObjectEntry proxy {};
                proxy.id = proxyId;
                proxy.comment = "PBXContainerItemProxy";
                proxy.isa = "PBXContainerItemProxy";
                proxy.fields = {
                    "containerPortal = " + refWithComment(rootProjectId, "Project object") + ";",
                    "proxyType = 1;",
                    "remoteGlobalIDString = " + depEntry.id + ";",
                    "remoteInfo = " + quoted(depEntry.name) + ";"
                };
                pbxContainerItemProxies.push_back(std::move(proxy));

                auto depId = makeID("PBXTargetDependency", entry.name + "->" + depEntry.name);
                ObjectEntry dependency {};
                dependency.id = depId;
                dependency.comment = depEntry.name;
                dependency.isa = "PBXTargetDependency";
                dependency.fields = {
                    "target = " + refWithComment(depEntry.id, depEntry.name) + ";",
                    "targetProxy = " + refWithComment(proxyId, "PBXContainerItemProxy") + ";"
                };
                pbxTargetDependencies.push_back(std::move(dependency));
                entry.dependencyRefs.push_back(refWithComment(depId, depEntry.name));

                if (entry.native && depEntry.native) {
                    auto depCompiled = std::dynamic_pointer_cast<CompiledTarget>(depEntry.target);
                    if (depCompiled->type == STATIC_LIBRARY || depCompiled->type == SHARED_LIBRARY) {
                        auto buildFileId = addBuildFileRef(depEntry.name, depEntry.productRefId);
                        entry.frameworkBuildFileRefs.push_back(refWithComment(buildFileId, depEntry.name + " in Frameworks"));
                    }
                }
            }
        }

        for (auto &entry : targets) {
            emitTargetConfig(entry, entry.native ? std::dynamic_pointer_cast<CompiledTarget>(entry.target).get() : nullptr);

            if (entry.native) {
                ObjectEntry sourcesPhase {};
                sourcesPhase.id = entry.sourcesPhaseId;
                sourcesPhase.comment = "Sources";
                sourcesPhase.isa = "PBXSourcesBuildPhase";
                sourcesPhase.fields = {
                    "buildActionMask = 2147483647;",
                    "files = " + formatArrayLiteral(entry.sourceBuildFileRefs, 4) + ";",
                    "runOnlyForDeploymentPostprocessing = 0;"
                };
                pbxSourcesBuildPhases.push_back(std::move(sourcesPhase));

                ObjectEntry frameworksPhase {};
                frameworksPhase.id = entry.frameworksPhaseId;
                frameworksPhase.comment = "Frameworks";
                frameworksPhase.isa = "PBXFrameworksBuildPhase";
                frameworksPhase.fields = {
                    "buildActionMask = 2147483647;",
                    "files = " + formatArrayLiteral(entry.frameworkBuildFileRefs, 4) + ";",
                    "runOnlyForDeploymentPostprocessing = 0;"
                };
                pbxFrameworkBuildPhases.push_back(std::move(frameworksPhase));

                ObjectEntry nativeTarget {};
                nativeTarget.id = entry.id;
                nativeTarget.comment = entry.name;
                nativeTarget.isa = "PBXNativeTarget";
                nativeTarget.fields = {
                    "buildConfigurationList = " + refWithComment(entry.configListId, "Build configuration list for PBXNativeTarget \"" + entry.name + "\"") + ";",
                    "buildPhases = " + formatArrayLiteral(entry.phaseRefs, 4) + ";",
                    "buildRules = ();",
                    "dependencies = " + formatArrayLiteral(entry.dependencyRefs, 4) + ";",
                    "name = " + quoted(entry.name) + ";",
                    "productName = " + quoted(entry.name) + ";",
                    "productReference = " + refWithComment(entry.productRefId, std::filesystem::path(entry.productPath).filename().string()) + ";",
                    "productType = " + quoted(entry.productType) + ";"
                };
                pbxNativeTargets.push_back(std::move(nativeTarget));
            } else {
                if (entry.script) {
                    auto scriptTarget = std::dynamic_pointer_cast<ScriptTarget>(entry.target);
                    std::vector<std::string> inputPaths = {quoted(projectRelativePathExpr(scriptTarget->script->value()))};
                    std::vector<std::string> outputPaths;
                    auto scriptOutputs = scriptTarget->outputs->toStringVector();
                    for (const auto &output : scriptOutputs) {
                        outputPaths.push_back(quoted(projectRelativePathExpr(output)));
                    }

                    ObjectEntry scriptPhase {};
                    scriptPhase.id = entry.shellScriptPhaseId;
                    scriptPhase.comment = "ShellScript";
                    scriptPhase.isa = "PBXShellScriptBuildPhase";
                    scriptPhase.fields = {
                        "buildActionMask = 2147483647;",
                        "files = ();",
                        "inputFileListPaths = ();",
                        "inputPaths = " + formatArrayLiteral(inputPaths, 4) + ";",
                        "name = " + quoted(entry.name) + ";",
                        "outputFileListPaths = ();",
                        "outputPaths = " + formatArrayLiteral(outputPaths, 4) + ";",
                        "runOnlyForDeploymentPostprocessing = 0;",
                        "shellPath = /bin/sh;",
                        "shellScript = " + quoted(buildShellScript(scriptTarget.get())) + ";",
                        "showEnvVarsInLog = 0;"
                    };
                    pbxShellScriptBuildPhases.push_back(std::move(scriptPhase));
                }

                ObjectEntry aggregateTarget {};
                aggregateTarget.id = entry.id;
                aggregateTarget.comment = entry.name;
                aggregateTarget.isa = "PBXAggregateTarget";
                aggregateTarget.fields = {
                    "buildConfigurationList = " + refWithComment(entry.configListId, "Build configuration list for PBXAggregateTarget \"" + entry.name + "\"") + ";",
                    "buildPhases = " + formatArrayLiteral(entry.phaseRefs, 4) + ";",
                    "dependencies = " + formatArrayLiteral(entry.dependencyRefs, 4) + ";",
                    "name = " + quoted(entry.name) + ";",
                    "productName = " + quoted(entry.name) + ";"
                };
                pbxAggregateTargets.push_back(std::move(aggregateTarget));
            }
        }

        emitProjectConfiguration();
        emitGroups();
        emitPBXProject(targets);

        pbxprojOut << "// !$*UTF8*$!\n";
        pbxprojOut << "{\n";
        pbxprojOut << "\tarchiveVersion = 1;\n";
        pbxprojOut << "\tclasses = {\n\t};\n";
        pbxprojOut << "\tobjectVersion = 46;\n";
        pbxprojOut << "\tobjects = {\n";

        writeSection("PBXBuildFile", pbxBuildFiles);
        writeSection("PBXContainerItemProxy", pbxContainerItemProxies);
        writeSection("PBXFileReference", pbxFileReferences);
        writeSection("PBXFrameworksBuildPhase", pbxFrameworkBuildPhases);
        writeSection("PBXGroup", pbxGroupObjects);
        writeSection("PBXNativeTarget", pbxNativeTargets);
        writeSection("PBXAggregateTarget", pbxAggregateTargets);
        writeSection("PBXProject", pbxProjectObjects);
        writeSection("PBXShellScriptBuildPhase", pbxShellScriptBuildPhases);
        writeSection("PBXSourcesBuildPhase", pbxSourcesBuildPhases);
        writeSection("PBXTargetDependency", pbxTargetDependencies);
        writeSection("XCBuildConfiguration", xcBuildConfigurations);
        writeSection("XCConfigurationList", xcConfigurationLists);

        pbxprojOut << "\t};\n";
        pbxprojOut << "\trootObject = " << refWithComment(rootProjectId, "Project object") << ";\n";
        pbxprojOut << "}\n";
        pbxprojOut.close();
    }
};

Gen *TargetXcode(OutputTargetOpts &outputOpts, GenXcodeOpts &genOpts) {
    return new XcodeGen(outputOpts, genOpts);
}

}
