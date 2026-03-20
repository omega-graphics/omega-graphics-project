#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <sstream>
#include <unordered_map>
#include "Gen.h"

namespace autom {

    // --- Step 1: Internal data structures ---

    struct VsProjectEntry {
        std::shared_ptr<Target> target;
        std::string guid;           // deterministic from name
        std::string name;
        std::string vcxprojPath;    // relative to output dir
        bool isCompiled;
    };

    // --- Step 2: GUID generation ---

    /// Generates a deterministic GUID from a seed string using SHA256.
    /// Output format: {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}
    static std::string generateGUID(const std::string &seed) {
        SHA256Hash hash;
        hash.addData((void *)seed.data(), seed.size());
        std::string hex;
        hash.getResultAsHex(hex);
        // Format first 32 hex chars as a GUID
        return "{" + hex.substr(0,8) + "-" + hex.substr(8,4) + "-"
             + hex.substr(12,4) + "-" + hex.substr(16,4) + "-"
             + hex.substr(20,12) + "}";
    }

    /// Maps TargetArch to Visual Studio platform string.
    static std::string vsPlatformString(TargetArch arch) {
        switch (arch) {
            case TargetArch::x86:    return "Win32";
            case TargetArch::x86_64: return "x64";
            case TargetArch::ARM:    return "ARM";
            case TargetArch::AARCH64: return "ARM64";
        }
        return "x64";
    }

    /// Maps AUTOM TargetType to Visual Studio ConfigurationType.
    static std::string vsConfigurationType(TargetType type) {
        switch (type) {
            case EXECUTABLE:      return "Application";
            case STATIC_LIBRARY:  return "StaticLibrary";
            case SHARED_LIBRARY:  return "DynamicLibrary";
            default:              return "Utility";
        }
    }

    // Standard C++ project type GUID
    static const char *VS_CPP_PROJECT_TYPE_GUID = "{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}";

    // --- Step 3: XML writing helpers ---

    static void xmlIndent(std::ostream &out, int indent) {
        for (int i = 0; i < indent; ++i)
            out << "  ";
    }

    static void xmlOpen(std::ostream &out, int indent, const std::string &tag, const std::string &attrs = "") {
        xmlIndent(out, indent);
        out << "<" << tag;
        if (!attrs.empty())
            out << " " << attrs;
        out << ">\n";
    }

    static void xmlClose(std::ostream &out, int indent, const std::string &tag) {
        xmlIndent(out, indent);
        out << "</" << tag << ">\n";
    }

    static void xmlEmpty(std::ostream &out, int indent, const std::string &tag, const std::string &attrs) {
        xmlIndent(out, indent);
        out << "<" << tag << " " << attrs << " />\n";
    }

    static void xmlElem(std::ostream &out, int indent, const std::string &tag, const std::string &content) {
        xmlIndent(out, indent);
        out << "<" << tag << ">" << content << "</" << tag << ">\n";
    }

    /// Joins a string vector with a separator.
    static std::string joinVec(const std::vector<std::string> &vec, const std::string &sep) {
        std::ostringstream s;
        for (size_t i = 0; i < vec.size(); ++i) {
            if (i > 0) s << sep;
            s << vec[i];
        }
        return s.str();
    }

    // --- Step 4: writeVcxproj for CompiledTarget ---

    class GenVisualStudio : public Gen {
        OutputTargetOpts & outputOpts;
        GenVisualStudioOpts & opts;

        std::ofstream solutionOut;
        std::vector<VsProjectEntry> pendingTargets;
        std::string platformStr;

        /// Maps raw Target pointer to index in pendingTargets for dep GUID lookup.
        std::unordered_map<Target *, size_t> targetIndexByPtr;

        /// Looks up the GUID for a dependency target. Returns empty string if not found.
        std::string guidForTarget(Target *t) {
            auto it = targetIndexByPtr.find(t);
            if (it != targetIndexByPtr.end())
                return pendingTargets[it->second].guid;
            return "";
        }

        // --- Step 5: writeVcxproj for ScriptTarget and FSTarget ---

        /// Writes the common vcxproj skeleton for Utility projects (Script/FS targets).
        /// The caller provides the command string and outputs for CustomBuildStep.
        void writeVcxprojUtility(const VsProjectEntry &entry,
                                 const std::string &command,
                                 const std::string &outputs,
                                 const std::string &description) {
            auto path = std::filesystem::path(context->outputDir.data()).append(entry.vcxprojPath);
            std::ofstream out(path, std::ios::out);

            out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
            xmlOpen(out, 0, "Project", "DefaultTargets=\"Build\" ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\"");

            // 1. Project configurations
            xmlOpen(out, 1, "ItemGroup", "Label=\"ProjectConfigurations\"");
            for (const auto &config : {"Debug", "Release"}) {
                std::string configPlatform = std::string(config) + "|" + platformStr;
                xmlOpen(out, 2, "ProjectConfiguration", "Include=\"" + configPlatform + "\"");
                xmlElem(out, 3, "Configuration", config);
                xmlElem(out, 3, "Platform", platformStr);
                xmlClose(out, 2, "ProjectConfiguration");
            }
            xmlClose(out, 1, "ItemGroup");

            // 2. Globals
            xmlOpen(out, 1, "PropertyGroup", "Label=\"Globals\"");
            xmlElem(out, 2, "ProjectGuid", entry.guid);
            xmlElem(out, 2, "RootNamespace", entry.name);
            xmlClose(out, 1, "PropertyGroup");

            xmlIndent(out, 1);
            out << "<Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n";

            // 3. ConfigurationType = Utility
            for (const auto &config : {"Debug", "Release"}) {
                std::string condition = "'$(Configuration)|$(Platform)'=='" + std::string(config) + "|" + platformStr + "'";
                xmlOpen(out, 1, "PropertyGroup", "Label=\"Configuration\" Condition=\"" + condition + "\"");
                xmlElem(out, 2, "ConfigurationType", "Utility");
                xmlElem(out, 2, "PlatformToolset", "v143");
                xmlClose(out, 1, "PropertyGroup");
            }

            xmlIndent(out, 1);
            out << "<Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n";

            // 4. CustomBuildStep
            xmlOpen(out, 1, "ItemDefinitionGroup");
            xmlOpen(out, 2, "CustomBuildStep");
            xmlElem(out, 3, "Command", command);
            xmlElem(out, 3, "Outputs", outputs);
            xmlElem(out, 3, "Message", description);
            xmlClose(out, 2, "CustomBuildStep");
            xmlClose(out, 1, "ItemDefinitionGroup");

            // 5. Project references for deps
            bool hasProjectRefs = false;
            for (const auto &dep : entry.target->resolvedDeps) {
                if (dep->type == GROUP_TARGET)
                    continue;
                std::string depGuid = guidForTarget(dep.get());
                if (depGuid.empty())
                    continue;
                if (!hasProjectRefs) {
                    xmlOpen(out, 1, "ItemGroup");
                    hasProjectRefs = true;
                }
                std::string depVcxproj = std::string(dep->name->value()) + ".vcxproj";
                xmlOpen(out, 2, "ProjectReference", "Include=\"" + depVcxproj + "\"");
                xmlElem(out, 3, "Project", depGuid);
                xmlClose(out, 2, "ProjectReference");
            }
            if (hasProjectRefs)
                xmlClose(out, 1, "ItemGroup");

            xmlIndent(out, 1);
            out << "<Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n";

            xmlClose(out, 0, "Project");
        }

        void writeVcxprojScript(const VsProjectEntry &entry) {
            auto script = std::dynamic_pointer_cast<ScriptTarget>(entry.target);

            // Build command: "script arg1 arg2 ..."
            std::ostringstream cmd;
            cmd << script->script->value();
            if (!script->args->empty()) {
                auto args = script->args->toStringVector();
                for (const auto &a : args)
                    cmd << " " << a;
            }

            // Outputs
            auto outputVec = script->outputs->toStringVector();
            std::string outputs = joinVec(outputVec, ";");

            std::string desc = std::string(script->desc->value());
            if (desc.empty())
                desc = "Running script: " + entry.name;

            writeVcxprojUtility(entry, cmd.str(), outputs, desc);
        }

        void writeVcxprojFS(const VsProjectEntry &entry) {
            auto fs = std::dynamic_pointer_cast<FSTarget>(entry.target);
            std::string command, outputs, desc;

            if (fs->type == FS_COPY) {
                auto sources = fs->sources->toStringVector();
                auto dest = std::string(fs->dest->value());
                std::ostringstream cmd;
                std::ostringstream outs;
                for (size_t i = 0; i < sources.size(); ++i) {
                    if (i > 0) cmd << " &amp;&amp; ";
                    cmd << "copy /Y \"" << sources[i] << "\" \"" << dest << "\"";
                    if (i > 0) outs << ";";
                    outs << dest << "\\" << std::filesystem::path(sources[i]).filename().string();
                }
                command = cmd.str();
                outputs = outs.str();
                desc = "Copy files to " + dest;
            }
            else if (fs->type == FS_MKDIR) {
                auto dest = std::string(fs->dest->value());
                command = "if not exist \"" + dest + "\" mkdir \"" + dest + "\"";
                outputs = dest;
                desc = "Create directory " + dest;
            }
            else if (fs->type == FS_SYMLINK) {
                auto dest = std::string(fs->dest->value());
                auto src = std::string(fs->symlink_src->value());
                command = "mklink \"" + dest + "\\" + std::filesystem::path(src).filename().string() + "\" \"" + src + "\"";
                outputs = dest + "\\" + std::filesystem::path(src).filename().string();
                desc = "Symlink " + src + " -> " + dest;
            }

            writeVcxprojUtility(entry, command, outputs, desc);
        }

        // --- Step 6: writeSolution ---

        void writeSolution() {
            // Header
            solutionOut << "\xEF\xBB\xBF\n"; // UTF-8 BOM
            solutionOut << "Microsoft Visual Studio Solution File, Format Version 12.00\n";
            solutionOut << "# Visual Studio Version 17\n";
            solutionOut << "VisualStudioVersion = 17.0.0.0\n";
            solutionOut << "MinimumVisualStudioVersion = 10.0.0.0\n";

            // Project entries (skip GroupTargets — they have no .vcxproj)
            for (const auto &entry : pendingTargets) {
                if (entry.target->type == GROUP_TARGET)
                    continue;
                solutionOut << "Project(\"" << VS_CPP_PROJECT_TYPE_GUID << "\") = \""
                            << entry.name << "\", \"" << entry.vcxprojPath << "\", \""
                            << entry.guid << "\"\n";
                solutionOut << "EndProject\n";
            }

            // Global section
            solutionOut << "Global\n";

            // Solution configuration platforms
            solutionOut << "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n";
            for (const auto &config : {"Debug", "Release"}) {
                solutionOut << "\t\t" << config << "|" << platformStr << " = "
                            << config << "|" << platformStr << "\n";
            }
            solutionOut << "\tEndGlobalSection\n";

            // Project configuration platforms
            solutionOut << "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n";
            for (const auto &entry : pendingTargets) {
                if (entry.target->type == GROUP_TARGET)
                    continue;
                for (const auto &config : {"Debug", "Release"}) {
                    std::string configPlat = std::string(config) + "|" + platformStr;
                    solutionOut << "\t\t" << entry.guid << "." << configPlat
                                << ".ActiveCfg = " << configPlat << "\n";
                    solutionOut << "\t\t" << entry.guid << "." << configPlat
                                << ".Build.0 = " << configPlat << "\n";
                }
            }
            solutionOut << "\tEndGlobalSection\n";

            solutionOut << "EndGlobal\n";
        }

        // --- Step 4: writeVcxproj for CompiledTarget ---

        void writeVcxprojCompiled(const VsProjectEntry &entry) {
            auto compiled = std::dynamic_pointer_cast<CompiledTarget>(entry.target);
            auto path = std::filesystem::path(context->outputDir.data()).append(entry.vcxprojPath);
            std::ofstream out(path, std::ios::out);

            out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
            xmlOpen(out, 0, "Project", "DefaultTargets=\"Build\" ToolsVersion=\"4.0\" xmlns=\"http://schemas.microsoft.com/developer/msbuild/2003\"");

            // 1. Project configurations
            xmlOpen(out, 1, "ItemGroup", "Label=\"ProjectConfigurations\"");
            for (const auto &config : {"Debug", "Release"}) {
                std::string configPlatform = std::string(config) + "|" + platformStr;
                xmlOpen(out, 2, "ProjectConfiguration", "Include=\"" + configPlatform + "\"");
                xmlElem(out, 3, "Configuration", config);
                xmlElem(out, 3, "Platform", platformStr);
                xmlClose(out, 2, "ProjectConfiguration");
            }
            xmlClose(out, 1, "ItemGroup");

            // 2. Globals
            xmlOpen(out, 1, "PropertyGroup", "Label=\"Globals\"");
            xmlElem(out, 2, "ProjectGuid", entry.guid);
            xmlElem(out, 2, "RootNamespace", entry.name);
            xmlClose(out, 1, "PropertyGroup");

            xmlIndent(out, 1);
            out << "<Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.Default.props\" />\n";

            // 3. Per-config type/toolset
            std::string configType = vsConfigurationType(compiled->type);
            for (const auto &config : {"Debug", "Release"}) {
                std::string condition = "'$(Configuration)|$(Platform)'=='" + std::string(config) + "|" + platformStr + "'";
                xmlOpen(out, 1, "PropertyGroup", "Label=\"Configuration\" Condition=\"" + condition + "\"");
                xmlElem(out, 2, "ConfigurationType", configType);
                xmlElem(out, 2, "PlatformToolset", "v143");
                if (std::string(config) == "Debug") {
                    xmlElem(out, 2, "UseDebugLibraries", "true");
                } else {
                    xmlElem(out, 2, "UseDebugLibraries", "false");
                }
                xmlClose(out, 1, "PropertyGroup");
            }

            xmlIndent(out, 1);
            out << "<Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.props\" />\n";

            // 4. Per-config ItemDefinitionGroup (compiler + linker settings)
            auto includes = compiled->include_dirs->toStringVector();
            auto cflags = compiled->cflags->toStringVector();
            auto defines = compiled->defines->toStringVector();
            auto libs = compiled->libs->toStringVector();
            auto libDirs = compiled->lib_dirs->toStringVector();
            auto ldflags = compiled->ldflags->toStringVector();

            // Add resolved library deps to libs list
            for (const auto &dep : compiled->resolvedDeps) {
                if (dep->type == SHARED_LIBRARY || dep->type == STATIC_LIBRARY) {
                    auto depCompiled = std::dynamic_pointer_cast<CompiledTarget>(dep);
                    std::string libName;
                    if (dep->type == SHARED_LIBRARY && depCompiled->implib_ext && !depCompiled->implib_ext->empty()) {
                        libName = std::string(dep->name->value()) + "." + std::string(depCompiled->implib_ext->value());
                    } else if (!depCompiled->output_ext->empty()) {
                        libName = std::string(dep->name->value()) + "." + std::string(depCompiled->output_ext->value());
                    }
                    if (!libName.empty())
                        libs.push_back(libName);

                    if (!depCompiled->output_dir->empty()) {
                        libDirs.push_back(std::string(depCompiled->output_dir->value()));
                    }
                }
            }

            for (const auto &config : {"Debug", "Release"}) {
                std::string condition = "'$(Configuration)|$(Platform)'=='" + std::string(config) + "|" + platformStr + "'";
                xmlOpen(out, 1, "ItemDefinitionGroup", "Condition=\"" + condition + "\"");

                // ClCompile
                xmlOpen(out, 2, "ClCompile");
                if (!includes.empty())
                    xmlElem(out, 3, "AdditionalIncludeDirectories", joinVec(includes, ";") + ";%(AdditionalIncludeDirectories)");
                if (!defines.empty())
                    xmlElem(out, 3, "PreprocessorDefinitions", joinVec(defines, ";") + ";%(PreprocessorDefinitions)");
                if (!cflags.empty())
                    xmlElem(out, 3, "AdditionalOptions", joinVec(cflags, " ") + " %(AdditionalOptions)");
                if (std::string(config) == "Debug") {
                    xmlElem(out, 3, "Optimization", "Disabled");
                    xmlElem(out, 3, "RuntimeLibrary", "MultiThreadedDebugDLL");
                } else {
                    xmlElem(out, 3, "Optimization", "MaxSpeed");
                    xmlElem(out, 3, "RuntimeLibrary", "MultiThreadedDLL");
                }
                xmlClose(out, 2, "ClCompile");

                // Link (only for exe and shared)
                if (compiled->type == EXECUTABLE || compiled->type == SHARED_LIBRARY) {
                    xmlOpen(out, 2, "Link");
                    if (!libs.empty())
                        xmlElem(out, 3, "AdditionalDependencies", joinVec(libs, ";") + ";%(AdditionalDependencies)");
                    if (!libDirs.empty())
                        xmlElem(out, 3, "AdditionalLibraryDirectories", joinVec(libDirs, ";") + ";%(AdditionalLibraryDirectories)");
                    if (!ldflags.empty())
                        xmlElem(out, 3, "AdditionalOptions", joinVec(ldflags, " ") + " %(AdditionalOptions)");
                    xmlClose(out, 2, "Link");
                }
                // Lib (for static library)
                else if (compiled->type == STATIC_LIBRARY) {
                    xmlOpen(out, 2, "Lib");
                    if (!ldflags.empty())
                        xmlElem(out, 3, "AdditionalOptions", joinVec(ldflags, " ") + " %(AdditionalOptions)");
                    xmlClose(out, 2, "Lib");
                }

                xmlClose(out, 1, "ItemDefinitionGroup");
            }

            // 4b. Output directory override
            if (!compiled->output_dir->empty() || !compiled->output_ext->empty()) {
                for (const auto &config : {"Debug", "Release"}) {
                    std::string condition = "'$(Configuration)|$(Platform)'=='" + std::string(config) + "|" + platformStr + "'";
                    xmlOpen(out, 1, "PropertyGroup", "Condition=\"" + condition + "\"");
                    if (!compiled->output_dir->empty())
                        xmlElem(out, 2, "OutDir", std::string(compiled->output_dir->value()) + "\\");
                    xmlElem(out, 2, "TargetName", entry.name);
                    if (!compiled->output_ext->empty())
                        xmlElem(out, 2, "TargetExt", "." + std::string(compiled->output_ext->value()));
                    xmlClose(out, 1, "PropertyGroup");
                }
            }

            // 5. Source files
            xmlOpen(out, 1, "ItemGroup");
            // Direct sources
            for (const auto &pair : compiled->source_object_map) {
                xmlEmpty(out, 2, "ClCompile", "Include=\"" + pair.first + "\"");
            }
            // Inlined SOURCE_GROUP sources
            for (const auto &dep : compiled->resolvedDeps) {
                if (dep->type == SOURCE_GROUP) {
                    auto sg = std::dynamic_pointer_cast<CompiledTarget>(dep);
                    for (const auto &pair : sg->source_object_map) {
                        xmlEmpty(out, 2, "ClCompile", "Include=\"" + pair.first + "\"");
                    }
                }
            }
            xmlClose(out, 1, "ItemGroup");

            // 6. Project references (compiled deps that are not SOURCE_GROUP)
            bool hasProjectRefs = false;
            for (const auto &dep : compiled->resolvedDeps) {
                if (IS_COMPILED_TARGET_TYPE(dep->type) && dep->type != SOURCE_GROUP) {
                    if (!hasProjectRefs) {
                        xmlOpen(out, 1, "ItemGroup");
                        hasProjectRefs = true;
                    }
                    std::string depGuid = guidForTarget(dep.get());
                    std::string depVcxproj = std::string(dep->name->value()) + ".vcxproj";
                    xmlOpen(out, 2, "ProjectReference", "Include=\"" + depVcxproj + "\"");
                    xmlElem(out, 3, "Project", depGuid);
                    xmlClose(out, 2, "ProjectReference");
                }
            }
            if (hasProjectRefs)
                xmlClose(out, 1, "ItemGroup");

            xmlIndent(out, 1);
            out << "<Import Project=\"$(VCTargetsPath)\\Microsoft.Cpp.targets\" />\n";

            xmlClose(out, 0, "Project");
        }

    public:
        GenVisualStudio(
                OutputTargetOpts & outputOpts,
                GenVisualStudioOpts & opts):
                outputOpts(outputOpts),opts(opts){
        };

        void configGenContext() override {
            solutionOut.open(std::filesystem::path(context->outputDir.data()).append(context->projectDesc.name).concat(".sln"));
            platformStr = vsPlatformString(outputOpts.arch);
        }

        void consumeToolchainDefaults(ToolchainDefaults &conf) override {

        }

        bool supportsCustomToolchainRules() override {
            return false;
        }

        void genToolchainRules(std::shared_ptr<Toolchain> &toolchain) override {

        }

        void consumeTarget(std::shared_ptr<Target> & target) override {
            VsProjectEntry entry;
            entry.target = target;
            entry.name = target->name->value();
            entry.guid = generateGUID(entry.name);
            entry.vcxprojPath = entry.name + ".vcxproj";
            entry.isCompiled = IS_COMPILED_TARGET_TYPE(target->type);
            pendingTargets.push_back(std::move(entry));
        }

        // --- Step 7: finish() orchestration ---

        void finish() override {
            // 1. Build pointer-to-index map for dep GUID resolution
            for (size_t i = 0; i < pendingTargets.size(); ++i) {
                targetIndexByPtr.insert({pendingTargets[i].target.get(), i});
            }

            // 2. Write .vcxproj for each target
            for (const auto &entry : pendingTargets) {
                if (entry.isCompiled) {
                    writeVcxprojCompiled(entry);
                }
                else if (entry.target->type == SCRIPT_TARGET) {
                    writeVcxprojScript(entry);
                }
                else if (IS_FS_ACTION_TYPE(entry.target->type)) {
                    writeVcxprojFS(entry);
                }
                // GROUP_TARGET: no .vcxproj emitted
            }

            // 3. Write .sln
            writeSolution();

            solutionOut.close();
        }
    };

    Gen *TargetVisualStudio(OutputTargetOpts & outputOpts,GenVisualStudioOpts & opts){
        return new GenVisualStudio(outputOpts,opts);
    }

}
