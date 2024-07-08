#include "AST.h"
#include <omegasl.h>
#include <memory>

#ifndef OMEGASL_CODEGEN_H
#define OMEGASL_CODEGEN_H

#define INTERFACE_FILENAME "interface.h"

namespace omegasl {

    struct CodeGenOpts {
        bool emitHeaderOnly;
        bool runtimeCompile;
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

        CodeGenOpts & opts;
        explicit CodeGen(CodeGenOpts & opts):
        opts(opts),
        typeResolver(nullptr)
//        interfaceGen(std::make_shared<InterfaceGen>(OmegaCommon::FS::Path(opts.outputLib).append(INTERFACE_FILENAME).absPath(), this))
        {

        }
        void setTypeResolver(ast::SemFrontend *_typeResolver){ typeResolver = _typeResolver;}
        virtual void generateDecl(ast::Decl *decl) = 0;
        virtual void generateExpr(ast::Expr *expr) = 0;
        virtual void generateBlock(ast::Block &block) = 0;
//        virtual void writeNativeStructDecl(ast::StructDecl *decl,std::ostream & out) = 0;
        void generateInterfaceAndCompileShader(ast::Decl *decl);

        /** @brief Compiles the shader with the provided name and outputs the compiled version to the output path provided.
         * @param type The Shader Type
         * @param name The Shader Name
         * @param path The source file location.
         * @param outputPath The output file location.
         * */
        virtual void compileShader(ast::ShaderDecl::Type type,const OmegaCommon::StrRef & name,const OmegaCommon::FS::Path & path,const OmegaCommon::FS::Path & outputPath) = 0;
        /** @brief Compiles the Shader with the provided name and outputs the compiled version to the shadermap.
         * @param type The Shader Type
         * @param name The Shader Name
         * @note
         * This function is only called when compiling omegasl on runtime.
         * */
        virtual void compileShaderOnRuntime(ast::ShaderDecl::Type type,const OmegaCommon::StrRef & name) = 0;
        void linkShaderObjects(){
            OmegaCommon::StrRef libname = OmegaCommon::FS::Path(opts.outputLib).dir();
            std::ofstream out(OmegaCommon::FS::Path(opts.outputLib).str(), std::ios::out | std::ios::binary);
            out.write((char *)&libname.size(),sizeof(OmegaCommon::StrRef::size_type));
            out.write(libname.data(),libname.size());
            unsigned int s = shaderMap.size();
            out.write((char *)&s,sizeof(s));

//            std::cout << "Write Shader Lib :" << libname.data() << std::endl;

            for(auto & p : shaderMap){
                auto & shader_data = p.second;
                //1.  Write Shader Type
                out.write((char *)&shader_data.type,sizeof(shader_data.type));
//                std::cout << "Write Shader :" << p.first << std::endl;

                //2.  Write Shader Name Size and Name
                size_t shader_name_size = strlen(p.second.name);

                out.write((char *)&shader_name_size,sizeof(shader_name_size));
                out.write(shader_data.name,shader_name_size);
//                std::cout << shader_data.name << std::endl;
//                std::cout << "Write Shader Name:" << shader_data.name << std::endl;
                //3.  Write Shader Data Size and Data
                {
                    std::ifstream in(p.first,std::ios::in | std::ios::binary);
                    if(in.is_open()){
                        in.seekg(0,std::ios::end);
                        size_t dataSize = in.tellg();
//                        std::cout <<  "Shader Len:" << dataSize << std::endl;
                        in.seekg(0,std::ios::beg);
                        out.write((char *)&dataSize,sizeof(dataSize));
                        for(size_t i = 0;i < dataSize;i++){
                            out << (char)in.get();
                        }
                        in.close();
                    }
                    else {
//                        std::cout << "Cannot find file:" << p.first << std::endl;
                        return;
                    }
                }

                //4. Write Shader Layout Length and Data
                if(shader_data.nLayout > 0){
                    OmegaCommon::ArrayRef<omegasl_shader_layout_desc> layoutDescArr {shader_data.pLayout,shader_data.pLayout + shader_data.nLayout};
                    out.write((char *)&shader_data.nLayout,sizeof(shader_data.nLayout));
                    for(auto & layout : layoutDescArr){
                        out.write((char *)&layout,sizeof(layout));
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
                        out.write(param.name,param_name_len);
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
        };
#ifdef RUNTIME_SHADER_COMP_SUPPORT
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
#endif
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
    std::shared_ptr<CodeGen> HLSLCodeGenMake(CodeGenOpts &opts,HLSLCodeOpts &hlslCodeOpts);
    std::shared_ptr<CodeGen> HLSLCodeGenMakeRuntime(CodeGenOpts &opts,HLSLCodeOpts &hlslCodeOpts,std::ostringstream & out);
    std::shared_ptr<CodeGen> MetalCodeGenMake(CodeGenOpts &opts,MetalCodeOpts &metalCodeOpts);
    std::shared_ptr<CodeGen> MetalCodeGenMakeRuntime(CodeGenOpts &opts,MetalCodeOpts &metalCodeOpts,std::ostringstream & out);


}

#endif