#include "AST.h"
#include "Target.h"
#include <omegasl.h>
#include <cstring>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <iostream>

#ifndef OMEGASL_CODEGEN_H
#define OMEGASL_CODEGEN_H

#define INTERFACE_FILENAME "interface.h"

namespace omegasl {

    struct CodeGenOpts {
        bool emitHeaderOnly;
        bool runtimeCompile;
        /// When set, write transpiled shader source to `tempDir` and stop —
        /// don't invoke the downstream toolchain (dxc / metal / glslc) and
        /// don't link a `.omegasllib`. Lets developers cross-target HLSL or
        /// GLSL from a non-Windows / non-Linux host for source-level
        /// debugging. Runtime correctness still has to be exercised on the
        /// matching platform.
        bool emitSourceOnly;
        OmegaCommon::StrRef outputLib;
        OmegaCommon::StrRef tempDir;
    };

    class InterfaceGen;

    struct CodeGen final {
    public:
        struct ResourceStore {
        private:
            typedef std::vector<ast::ResourceDecl *> data ;
            data resources;
        public:
            inline void add(ast::ResourceDecl * res){
                resources.push_back(res);
            }
            inline data::iterator begin(){
                return resources.begin();
            }
            inline data::iterator find(const OmegaCommon::StrRef & name){
                using namespace OmegaCommon;
                auto it = resources.begin();
                for(;it != resources.end();it++){
                    auto & item = *it;
                    if(item->name == name){
                        break;
                    }
                }
                return it;
            };
            inline data::iterator end(){
                return resources.end();
            }
        };

        ResourceStore resourceStore;

        ast::SemFrontend *typeResolver;
        std::shared_ptr<InterfaceGen> interfaceGen;

        OmegaCommon::Map<OmegaCommon::String,omegasl_shader> shaderMap;

        /// Names (object_file map keys) of shaders whose required-feature
        /// set could not be expressed by the active backend. Their
        /// `omegasl_shader` entry holds only the header (type, name,
        /// requiredFeatures); no source file was written, no compilation
        /// was invoked. `linkShaderObjects` writes a `dataSize == 0`
        /// record for each so the runtime can produce a precise rejection
        /// diagnostic when a pipeline names the shader. See
        /// OmegaSL-Feature-Gap-Survey §14.1 (user-requested twist:
        /// transpile as null + tag the bitfield, no compile-time hard-fail).
        std::set<std::string> stubShaderKeys;

        /// File-scope `#requires(...)` bits the Preprocessor accumulated
        /// for the source currently being processed. Every shader emitted
        /// from this source carries this bitfield in its `omegasl_shader`
        /// record so the runtime loader can reject it on devices that
        /// don't satisfy the requirements.
        uint64_t fileRequiredFeatures = 0;
        /// Subset of `fileRequiredFeatures` whose `OMEGASL_FEATURE_<NAME>`
        /// macro was not defined for the active backend. A non-zero value
        /// triggers the stub-emission path in SHADER_DECL.
        uint64_t fileUnsatisfiedFeatures = 0;

        void setRequiredFeatures(uint64_t required, uint64_t unsatisfied) {
            fileRequiredFeatures = required;
            fileUnsatisfiedFeatures = unsatisfied;
        }

        /// Names of user-defined functions encountered during parsing.
        /// Populated by each backend's FUNC_DECL handler. Used by the
        /// shared CALL_EXPR / FUNC_DECL emission paths to recognize a
        /// user function and write its `osl_user_<name>` spelling at the
        /// call/definition site.
        OmegaCommon::Vector<ast::FuncDecl *> userFuncDecls;
        std::set<std::string> userFuncNames;

        /// §3.5 — count of how many user FuncDecls share each name.
        /// Populated lazily by `userFuncNameCount` from `userFuncDecls`.
        /// Drives the overload-mangling decision in `spellUserFuncName`:
        /// names that appear more than once get a parameter-type suffix
        /// so each overload emits a unique target-language symbol.
        mutable std::map<std::string, int> userFuncNameCounts_;
        mutable bool userFuncNameCountsBuilt_ = false;

        int userFuncNameCount(OmegaCommon::StrRef name) const {
            if (!userFuncNameCountsBuilt_) {
                for (auto *fd : userFuncDecls) {
                    userFuncNameCounts_[std::string(fd->name)]++;
                }
                userFuncNameCountsBuilt_ = true;
            }
            auto it = userFuncNameCounts_.find(std::string(name));
            return it == userFuncNameCounts_.end() ? 0 : it->second;
        }

        /// AST pointers for every `ShaderDecl` the parser handed off to
        /// `generateDecl`. Collected so the post-parse portability
        /// scanner (Feature-Gap-Survey §14.2) can revisit them after
        /// every user function is known — handles forward references
        /// where a shader calls a user function defined later in the
        /// file. Not used by codegen itself.
        OmegaCommon::Vector<ast::ShaderDecl *> shaderDecls;

        /// Prefix used when a user function name collides with a
        /// target-specific stdlib identifier. The prefix is stable so
        /// `osl_user_*` is reliably namespaced in generated source.
        static OmegaCommon::String mangleUserFuncName(OmegaCommon::StrRef name) {
            return std::string("osl_user_") + std::string(name);
        }

        /// §3.5 — overload-aware mangling. When a user function name
        /// has more than one declaration in the current translation
        /// unit, every emit site must produce a unique target-language
        /// symbol or the downstream compiler sees a redefinition. The
        /// suffix is built from the parameter list using each type's
        /// canonical OmegaSL spelling joined with underscores; pointer
        /// parameters get a trailing `_p` marker. The leading double
        /// underscore separates the function-name from the suffix so
        /// debugging output stays readable. Example: `myLerp(float,
        /// float, float)` → `osl_user_myLerp__float_float_float`.
        ///
        /// Takes the positional `Vector<TypeExpr*>` rather than the
        /// FuncType's unordered `fields` map — order is observable in
        /// the mangled symbol, and `MapVec` is a hash table.
        static OmegaCommon::String mangleUserFuncName(
            OmegaCommon::StrRef name,
            const OmegaCommon::Vector<ast::TypeExpr *> &paramTypes) {
            std::string out = "osl_user_";
            out += std::string(name);
            out += "__";
            bool first = true;
            for (auto *t : paramTypes) {
                if (!first) out += "_";
                first = false;
                out += std::string(t->name);
                if (t->pointer) out += "_p";
            }
            return out;
        }

        bool isUserFunc(OmegaCommon::StrRef name) const {
            return userFuncNames.count(std::string(name)) > 0;
        }

        /// Returns the spelling to write when emitting a user-defined
        /// function name (in either its definition or a call site).
        /// Every user function is prefixed with `osl_user_` on every
        /// backend, unconditionally. A curated per-backend stdlib list
        /// can never be complete — Metal's namespace alone spans the
        /// math, geometric, and `<metal_type_traits>` surfaces (e.g.
        /// `add_const`), so any name the list misses becomes a
        /// platform-dependent collision. Prefixing always removes the
        /// entire class of failures and keeps the generated symbol for
        /// a given OmegaSL function identical across HLSL/MSL/GLSL.
        /// Shader entry points (vertex/fragment/compute) are emitted
        /// through a different path and keep their bare names — the
        /// runtime looks them up by the source name.
        OmegaCommon::String spellUserFuncName(OmegaCommon::StrRef name) const {
            return mangleUserFuncName(name);
        }

        /// §3.5 — overload-aware spelling. Used at sites that know the
        /// resolved parameter list (the FUNC_DECL emit path uses the
        /// FuncDecl's params; the CALL_EXPR emit path uses the
        /// CallExpr's `resolvedCallee->paramTypes`). Single-overload
        /// names route through the simple form so the common case
        /// stays clean and only collisions get prefixed.
        OmegaCommon::String spellUserFuncName(
            OmegaCommon::StrRef name,
            const OmegaCommon::Vector<ast::TypeExpr *> &paramTypes) const {
            if (userFuncNameCount(name) > 1) {
                return mangleUserFuncName(name, paramTypes);
            }
            return spellUserFuncName(name);
        }

        /// Set by a backend when it encounters a construct it cannot
        /// emit (e.g. Metal hull/domain stages — see OmegaSL-Reference.md
        /// bug 3). The backend prints its own diagnostic to stderr and
        /// sets this flag; the driver checks it after parsing and exits
        /// nonzero. Lets us reject unsupported stages cleanly without
        /// emitting bogus target source that the downstream compiler
        /// then rejects with a confusing error.
        bool hasFatalErrors = false;

        CodeGenOpts & opts;
    protected:
        /// The backend-specific decision points the AST walk consults.
        /// Phase 0: owned but not yet called — subsequent phases relocate
        /// per-backend code into hooks on Target. See
        /// docs/OmegaSL-CodeGen-Target-Refactor-Plan.md.
        std::unique_ptr<Target> target;

        /// Phase 10: shader source output. Two modes:
        ///   - Offline (`opts.runtimeCompile == false`): each SHADER_DECL
        ///     opens `fileOut` to a per-shader file under `opts.tempDir`
        ///     and the source is written there. `runtimeStringOut` is
        ///     null.
        ///   - Runtime: the caller supplies an external `std::ostringstream`
        ///     in `CreateRuntime`; we keep a non-owning pointer in
        ///     `runtimeStringOut` and clear/append per shader.
        /// `shaderOut` is the active stream — a reference set in the
        /// constructor pointing at either `fileOut` or the supplied
        /// runtime ostringstream. `compileShaderOnRuntime` reads
        /// `runtimeStringOut->str()` to feed the in-process compiler.
        std::ofstream fileOut;
        std::ostringstream *runtimeStringOut = nullptr;
        std::ostream &shaderOut;

        /// §2.3 Phase B — statement-injection redirect. When non-null,
        /// `shaderOutStream()` returns this stream instead of `shaderOut`.
        /// `generateBlock` points it at a per-statement scratch buffer while
        /// rendering each statement, so a backend whose expression emission
        /// needs preceding statements (HLSL `GetDimensions`, which uses
        /// out-params and cannot be a sub-expression) can queue those lines
        /// via `queuePendingStatement`. When null — the common case — output
        /// is byte-identical to the pre-Phase-B walk. `renderExprToString`
        /// also borrows it to spell a sub-expression off-stream.
        std::ostream *outputRedirect_ = nullptr;
    public:
        Target *getTarget() { return target.get(); }
        std::ostream &getShaderOut() { return shaderOut; }

        explicit CodeGen(CodeGenOpts & opts, std::unique_ptr<Target> target):
        typeResolver(nullptr),
        opts(opts),
        target(std::move(target)),
        shaderOut(fileOut)
//        interfaceGen(std::make_shared<InterfaceGen>(OmegaCommon::FS::Path(opts.outputLib).append(INTERFACE_FILENAME).absPath(), this))
        {

        }
        explicit CodeGen(CodeGenOpts & opts, std::unique_ptr<Target> target, std::ostringstream &out):
        typeResolver(nullptr),
        opts(opts),
        target(std::move(target)),
        runtimeStringOut(&out),
        shaderOut(out)
        {

        }

        ~CodeGen() = default;

        void setTypeResolver(ast::SemFrontend *_typeResolver){ typeResolver = _typeResolver;}

        /// Phase 10: shared, non-virtual decl walk. Per-target divergence
        /// flows through `Target::*` hooks (struct decls, var decls,
        /// return decls, shader entry, resource binding, ...). The
        /// previous per-backend `generateDecl` overrides have been
        /// folded in here.
        void generateDecl(ast::Decl *decl);

        /// Phase 10: spell a `TypeExpr` for the active target. Replaces
        /// the per-subclass `writeTypeExpr` helpers.
        void writeTypeExpr(ast::TypeExpr *typeExpr, std::ostream &out);

        /// Phase 10: shared user-function emission. The bodies were
        /// identical across the three backends; promoted onto `CodeGen`
        /// alongside `generateDecl`.
        void emitUserFunctionSignature(ast::FuncDecl *f);
        void emitUserFunctionPrototype(ast::FuncDecl *f);
        void emitUserFunction(ast::FuncDecl *f);

        /// Concrete shared AST-walk for expression nodes. After Phase 7.5
        /// + 8a the body is identical across HLSL/MSL/GLSL modulo Target
        /// hook calls, so it lives on the shared base. The output stream
        /// is fetched per-subclass via `shaderOutStream()` until Phase 10
        /// folds the file/string members up here too.
        void generateExpr(ast::Expr *expr);

        /// Concrete shared AST-walk for blocks. After Phase 8c+8d the
        /// per-backend bodies converged on the same shape: `{`, then
        /// each stmt at indent+1 with maybe-`;` + `\n`, then `}`. The
        /// MSL pre-Phase-8d quirk that indented `{` after `if`/`for`/
        /// `while` (e.g. `if(...)    {`) is gone — output now matches
        /// the HLSL/GLSL form `if(...){`. Indent level still bumps via
        /// `indentLevel`, so nested blocks indent correctly relative to
        /// their parent.
        void generateBlock(ast::Block &block);

        /// §2.3 Phase B — emit one statement at the current indent level,
        /// honoring statement injection. The statement is rendered into a
        /// scratch buffer (with the per-statement redirect active) so a
        /// backend can queue lines to be emitted *before* it; queued lines
        /// are flushed (indented) ahead of the statement, then the statement
        /// itself with its trailing `;` (non-block statements only).
        /// `pendingStatements` is swapped out for the render so lines from an
        /// enclosing statement survive. Shared by `generateBlock` and the
        /// `switch`-case-body loop in `generateDecl`. Byte-identical to the
        /// pre-Phase-B per-statement emission when nothing is queued.
        void emitStatementLine(ast::Stmt *stmt);

        /// Phase 10: output stream accessor used by `generateExpr` /
        /// `generateBlock`. Always returns the `shaderOut` reference
        /// established in the constructor (file in offline mode, the
        /// runtime caller's ostringstream in runtime mode).
        std::ostream &shaderOutStream() { return outputRedirect_ ? *outputRedirect_ : shaderOut; }

        /// §2.3 Phase B — lines a Target queued during expression emission,
        /// to be written (indented to the current level) immediately before
        /// the statement being rendered. Drained per-statement by
        /// `generateBlock`. Empty in the common case.
        std::vector<std::string> pendingStatements;

        /// §2.3 Phase B — monotonic id for HLSL `GetDimensions` temporaries
        /// (`_gd<N>_w`, ...) so multiple queries in one shader don't collide.
        unsigned getDimensionsTempId = 0;

        void queuePendingStatement(std::string stmt) {
            pendingStatements.push_back(std::move(stmt));
        }

        /// Spell `expr` to a string using the active target, without writing
        /// to the live output stream. Used by a backend that needs the
        /// spelled form of a sub-expression to build an injected statement
        /// (HLSL `GetDimensions` — see `emitTextureGetDimensions`).
        std::string renderExprToString(ast::Expr *expr) {
            std::ostringstream tmp;
            std::ostream *saved = outputRedirect_;
            outputRedirect_ = &tmp;
            generateExpr(expr);
            outputRedirect_ = saved;
            return tmp.str();
        }

        /// Current block-nesting depth, in indentation levels (one
        /// level == 4 spaces after Phase 7.5 unification). Each
        /// `generateBlock` call increments at entry and decrements at
        /// exit. `Target::emitShaderEntryBody` overrides also bump it
        /// when emitting the entry body so nested control-flow blocks
        /// indent relative to the entry, not relative to file scope.
        unsigned indentLevel = 0;

        /// Run the per-resource emission loop and fill `meta.pLayout`
        /// (allocated as `new[]`) with the resulting layout descriptors.
        /// Each backend's `emitShaderEntryHeader` calls this where
        /// resources land in its source: HLSL/GLSL at file scope before
        /// the function signature, MSL inline inside the entry-function
        /// parameter list. The shared `SHADER_DECL` no longer drives the
        /// loop directly.
        void emitResourcesAndFillLayout(ast::ShaderDecl *decl,
                                        omegasl_shader &meta,
                                        std::ostream &out);
//        virtual void writeNativeStructDecl(ast::StructDecl *decl,std::ostream & out) = 0;
        bool generateInterfaceAndCompileShader(ast::Decl *decl);

        /** @brief Compiles the shader with the provided name and outputs the compiled version to the output path provided.
         * @param type The Shader Type
         * @param name The Shader Name
         * @param path The source file location.
         * @param outputPath The output file location.
         * Phase 10: thin delegation to `target->compileShader`.
         * */
        bool compileShader(ast::ShaderDecl::Type type,const OmegaCommon::StrRef & name,const OmegaCommon::FS::Path & path,const OmegaCommon::FS::Path & outputPath) {
            /// `fileRequiredFeatures` is the file-scope `#requires(...)`
            /// bitfield; HLSL reads it to bump the dxc target profile to
            /// SM 6.2 + `-enable-16bit-types` when FLOAT16 is declared.
            return target->compileShader(type, name, fileRequiredFeatures, path, outputPath);
        }
        /** @brief Compiles the Shader with the provided name and outputs the compiled version to the shadermap.
         * @param type The Shader Type
         * @param name The Shader Name
         * @note
         * This function is only called when compiling omegasl on runtime.
         * Phase 10: thin delegation to `target->compileShaderRuntime`,
         * passing the captured runtime source buffer.
         * */
        void compileShaderOnRuntime(ast::ShaderDecl::Type type,const OmegaCommon::StrRef & name) {
            OmegaCommon::String shaderName{name.begin(), name.end()};
            auto it = shaderMap.find(shaderName);
            if (it == shaderMap.end()) {
                std::cerr << "Runtime compile: shader entry not found for `" << shaderName << "`" << std::endl;
                return;
            }
            std::string source = runtimeStringOut ? runtimeStringOut->str() : std::string{};
            /// Read the per-shader requirements bit off the meta record
            /// the SHADER_DECL handler stamped earlier — same source of
            /// truth as the offline path's `fileRequiredFeatures`.
            target->compileShaderRuntime(type, name, it->second.requiredFeatures, source, it->second);
        }
        bool linkShaderObjects(){
            auto outputPath = OmegaCommon::FS::Path(opts.outputLib);
            OmegaCommon::String libname = outputPath.filename();
            std::ofstream out(outputPath.str(), std::ios::out | std::ios::binary);
            if(!out.is_open()){
                std::cerr << "error: cannot create output library: " << outputPath.str() << std::endl;
                return false;
            }
            size_t libname_size = libname.size();
            out.write((char *)&libname_size,sizeof(libname_size));
            out.write(libname.data(),libname.size());
            unsigned int s = shaderMap.size();
            out.write((char *)&s,sizeof(s));

            for(auto & p : shaderMap){
                auto & shader_data = p.second;
                /// Layer 1 stub path: the entry was recorded with no
                /// backing object file because the active backend cannot
                /// express one of the declared `#requires(...)` features.
                /// Write a `dataSize == 0` record (header-only) plus the
                /// `requiredFeatures` bitfield so the runtime can produce
                /// a precise rejection diagnostic.
                bool isStub = stubShaderKeys.count(std::string(p.first)) > 0;

                std::ifstream in;
                if(!isStub){
                    //0.  Pre-check: verify compiled shader object is readable before writing anything
                    in.open(p.first,std::ios::in | std::ios::binary);
                    if(!in.is_open()){
                        std::cerr << "error: cannot open compiled shader object: " << p.first << std::endl;
                        return false;
                    }
                }

                //1.  Write Shader Type
                out.write((char *)&shader_data.type,sizeof(shader_data.type));

                //2.  Write Shader Name Size and Name
                size_t shader_name_size = strlen(p.second.name);
                out.write((char *)&shader_name_size,sizeof(shader_name_size));
                out.write(shader_data.name,std::streamsize(shader_name_size));

                //3.  Write Shader Data Size and Data
                if(isStub){
                    size_t dataSize = 0;
                    out.write((char *)&dataSize,sizeof(dataSize));
                }
                else {
                    in.seekg(0,std::ios::end);
                    size_t dataSize = in.tellg();
                    in.seekg(0,std::ios::beg);
                    out.write((char *)&dataSize,sizeof(dataSize));
                    for(size_t i = 0;i < dataSize;i++){
                        out << (char)in.get();
                    }
                    in.close();
                }

                //4. Write Shader Layout Length and Data
                if(shader_data.nLayout > 0){
                    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutDescArr {shader_data.pLayout,shader_data.pLayout + shader_data.nLayout};
                    out.write((char *)&shader_data.nLayout,sizeof(shader_data.nLayout));
                    for(auto & layout : layoutDescArr){
                        // Zero-fill a serialization buffer so struct/union padding bytes are
                        // deterministic on disk. The source struct may carry uninitialized
                        // padding from heap allocation; copying it byte-wise leaks that.
                        omegasl_shader_layout_desc serializedLayout;
                        std::memset(&serializedLayout, 0, sizeof(serializedLayout));
                        serializedLayout.type = layout.type;
                        serializedLayout.gpu_relative_loc = layout.gpu_relative_loc;
                        serializedLayout.io_mode = layout.io_mode;
                        serializedLayout.location = layout.location;
                        serializedLayout.offset = layout.offset;
                        serializedLayout.sampler_desc.filter = layout.sampler_desc.filter;
                        serializedLayout.sampler_desc.u_address_mode = layout.sampler_desc.u_address_mode;
                        serializedLayout.sampler_desc.v_address_mode = layout.sampler_desc.v_address_mode;
                        serializedLayout.sampler_desc.w_address_mode = layout.sampler_desc.w_address_mode;
                        serializedLayout.sampler_desc.max_anisotropy = layout.sampler_desc.max_anisotropy;
                        // constant_desc intentionally left zero: no codegen populates it today.
                        out.write((char *)&serializedLayout,sizeof(serializedLayout));
                    }
                }
                else {
                    unsigned int len = 0;
                    out.write((char *)&len,sizeof(len));
                }

                /// Stage-specific decoration only travels with shaders
                /// that have a real body — stubs have no parameters /
                /// threadgroup info to record.
                if(!isStub){
                    if(shader_data.type == OMEGASL_SHADER_VERTEX) {

                        /// 5. (For Vertex Shaders) Write Shader Vertex Input Desc
                        out.write((char *) &shader_data.vertexShaderInputDesc.useVertexID,
                                  sizeof(shader_data.vertexShaderInputDesc.useVertexID));
                        out.write((char *) &shader_data.vertexShaderInputDesc.nParam,
                                  sizeof(shader_data.vertexShaderInputDesc.nParam));
                        OmegaCommon::ArrayRef<omegasl_vertex_shader_param_desc> vertexShaderParamDescArr{
                                shader_data.vertexShaderInputDesc.pParams,
                                shader_data.vertexShaderInputDesc.pParams + shader_data.vertexShaderInputDesc.nParam};
                        for (auto &param: vertexShaderParamDescArr) {
                            size_t param_name_len = strlen(param.name);
                            out.write((char *)&param_name_len,sizeof(param_name_len));
                            out.write(param.name,std::streamsize(param_name_len));
                            out.write((char *)&param.type,sizeof(param.type));
                            out.write((char *)&param.offset,sizeof(param.offset));
                        }
                    }
                    else if(shader_data.type == OMEGASL_SHADER_COMPUTE){
                        out.write((char *)&shader_data.threadgroupDesc.x,sizeof(unsigned int));
                        out.write((char *)&shader_data.threadgroupDesc.y,sizeof(unsigned int));
                        out.write((char *)&shader_data.threadgroupDesc.z,sizeof(unsigned int));
                    }
                }

                /// 6. Per-shader required feature bitfield (Layer 1 of
                /// the Backend Feature Gating system, §14.1/§14.3). The
                /// runtime loader masks this against the device feature
                /// bitmask and rejects only the shaders whose required
                /// bits are not satisfied. Always written, even when 0.
                out.write((char *)&shader_data.requiredFeatures,
                          sizeof(shader_data.requiredFeatures));
            }

            out.close();
            return true;
        };
        std::shared_ptr<omegasl_shader_lib> getLibrary(OmegaCommon::StrRef name){
            auto res = std::make_shared<omegasl_shader_lib>();
            res->header.name = name.data();
            res->header.name_length = name.size();
            res->header.entry_count = shaderMap.size();
            res->shaders = new omegasl_shader [shaderMap.size()];
            unsigned idx = 0;
            for(auto & s_pair : shaderMap){
                memcpy(res->shaders + idx,&s_pair.second,sizeof(omegasl_shader));
                idx += 1;
            }
            return res;
        }
        void resetShaderMap(){
            shaderMap.clear();
        }
    };

//    class InterfaceGen final {
//        std::ofstream out;
//        CodeGen *gen;
//    public:
//        InterfaceGen(OmegaCommon::String filename,CodeGen *gen):gen(gen){
//            out.open(filename);
//            out << "// Warning! This file was generated by omegaslc. DO NOT EDIT!" << std::endl <<
//            "#include \"omegaGTE/GTEBase.h\"" << std::endl;
//        }
//        inline void writeCrossType(ast::TypeExpr *t){
//            using namespace ast;
//            auto *_t = gen->typeResolver->resolveTypeWithExpr(t);
//            if(_t == builtins::void_type){
//                out << "void";
//            }
//            else if(_t == builtins::float_type){
//                out << "float";
//            }
//            else if(_t == builtins::float2_type){
//                out << "FVec<2>";
//            }
//            else if(_t == builtins::float3_type){
//                out << "FVec<3>";
//            }
//            else if(_t == builtins::float4_type){
//                out << "FVec<4>";
//            }
//            if(t->pointer){
//                out << " *";
//            }
//        }
//        void generateStruct(ast::StructDecl *decl){
//            if(!decl->internal){
//                out << "struct " << decl->name << " {" << std::endl;
//                for(auto p : decl->fields){
//                    out << "    ";
//                    writeCrossType(p.typeExpr);
//                    out << " " << p.name;
//                    out << ";" << std::endl;
//                }
//                out << "};" << std::endl;
//                #if defined(TARGET_DIRECTX)
//                    out << "#ifdef TARGET_DIRECTX";
//                #elif defined(TARGET_METAL)
//                    out << "#ifdef TARGET_METAL";
//                #elif defined(TARGET_VULKAN)
//                    out << "#ifdef TARGET_VULKAN";
//                #endif
//                out << std::endl;
//                gen->writeNativeStructDecl(decl,out);
//                out << "#endif" << std::endl << std::endl;
//            }
//        }
//        ~InterfaceGen(){
//            out.close();
//        }
//    };

    struct GLSLCodeOpts {
        OmegaCommon::String glslc_cmd;
    };

    struct HLSLCodeOpts {
        OmegaCommon::String dxc_cmd;
    };

    struct MetalCodeOpts {
        OmegaCommon::String metal_cmd;
        void *mtl_device = nullptr;
    };


    /// Phase 10: single factory per backend, replacing the three
    /// HLSL/MSL/GLSL `*CodeGenMake[Runtime]` pairs. The caller picks the
    /// backend by constructing the matching `Target` subclass and passing
    /// it in.
    std::shared_ptr<CodeGen> CodeGenMake(CodeGenOpts &opts, std::unique_ptr<Target> target);
    std::shared_ptr<CodeGen> CodeGenMakeRuntime(CodeGenOpts &opts, std::unique_ptr<Target> target,
                                                std::ostringstream &out);


}

#endif