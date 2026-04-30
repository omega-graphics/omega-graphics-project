#include "AST.h"
#include "Target.h"
#include <omegasl.h>
#include <cstring>
#include <memory>
#include <set>
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

    struct CodeGen {
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

        /// Names of user-defined functions encountered during parsing.
        /// Populated by each backend's FUNC_DECL handler. Used by the
        /// shared CALL_EXPR / FUNC_DECL emission paths together with
        /// `Target::needsMangling` to decide whether to write the bare
        /// name or `osl_user_<name>` at the call/definition site.
        OmegaCommon::Vector<ast::FuncDecl *> userFuncDecls;
        std::set<std::string> userFuncNames;

        /// Prefix used when a user function name collides with a
        /// target-specific stdlib identifier. The prefix is stable so
        /// `osl_user_*` is reliably namespaced in generated source.
        static OmegaCommon::String mangleUserFuncName(OmegaCommon::StrRef name) {
            return std::string("osl_user_") + std::string(name);
        }

        bool isUserFunc(OmegaCommon::StrRef name) const {
            return userFuncNames.count(std::string(name)) > 0;
        }

        /// Returns the spelling to write when emitting a user-defined
        /// function name (in either its definition or a call site). Only
        /// mangles when the target has flagged the name as colliding
        /// with one of its own stdlib identifiers. Non-colliding user
        /// names pass through unchanged so the generated source stays
        /// readable.
        OmegaCommon::String spellUserFuncName(OmegaCommon::StrRef name) const {
            if (target->needsMangling(name)) {
                return mangleUserFuncName(name);
            }
            return std::string(name);
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
    public:
        explicit CodeGen(CodeGenOpts & opts, std::unique_ptr<Target> target):
        typeResolver(nullptr),
        opts(opts),
        target(std::move(target))
//        interfaceGen(std::make_shared<InterfaceGen>(OmegaCommon::FS::Path(opts.outputLib).append(INTERFACE_FILENAME).absPath(), this))
        {

        }

        virtual ~CodeGen() = default;

        void setTypeResolver(ast::SemFrontend *_typeResolver){ typeResolver = _typeResolver;}
        virtual void generateDecl(ast::Decl *decl) = 0;

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

        /// Per-subclass output stream. Resolves to either the on-disk
        /// `fileOut` or the in-memory `stringOut` depending on the
        /// constructor variant the subclass was built with.
        virtual std::ostream &shaderOutStream() = 0;

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
         * */
        virtual bool compileShader(ast::ShaderDecl::Type type,const OmegaCommon::StrRef & name,const OmegaCommon::FS::Path & path,const OmegaCommon::FS::Path & outputPath) = 0;
        /** @brief Compiles the Shader with the provided name and outputs the compiled version to the shadermap.
         * @param type The Shader Type
         * @param name The Shader Name
         * @note
         * This function is only called when compiling omegasl on runtime.
         * */
        virtual void compileShaderOnRuntime(ast::ShaderDecl::Type type,const OmegaCommon::StrRef & name) = 0;
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

                //0.  Pre-check: verify compiled shader object is readable before writing anything
                std::ifstream in(p.first,std::ios::in | std::ios::binary);
                if(!in.is_open()){
                    std::cerr << "error: cannot open compiled shader object: " << p.first << std::endl;
                    return false;
                }

                //1.  Write Shader Type
                out.write((char *)&shader_data.type,sizeof(shader_data.type));

                //2.  Write Shader Name Size and Name
                size_t shader_name_size = strlen(p.second.name);
                out.write((char *)&shader_name_size,sizeof(shader_name_size));
                out.write(shader_data.name,std::streamsize(shader_name_size));

                //3.  Write Shader Data Size and Data
                {
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


    std::shared_ptr<CodeGen> GLSLCodeGenMake(CodeGenOpts &opts,GLSLCodeOpts &glslCodeOpts);
    std::shared_ptr<CodeGen> GLSLCodeGenMakeRuntime(CodeGenOpts &opts,GLSLCodeOpts &glslCodeOpts,std::ostringstream & out);
    std::shared_ptr<CodeGen> HLSLCodeGenMake(CodeGenOpts &opts,HLSLCodeOpts &hlslCodeOpts);
    std::shared_ptr<CodeGen> HLSLCodeGenMakeRuntime(CodeGenOpts &opts,HLSLCodeOpts &hlslCodeOpts,std::ostringstream & out);
    std::shared_ptr<CodeGen> MetalCodeGenMake(CodeGenOpts &opts,MetalCodeOpts &metalCodeOpts);
    std::shared_ptr<CodeGen> MetalCodeGenMakeRuntime(CodeGenOpts &opts,MetalCodeOpts &metalCodeOpts,std::ostringstream & out);


}

#endif