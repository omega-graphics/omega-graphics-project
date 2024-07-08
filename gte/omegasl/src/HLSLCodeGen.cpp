#include "CodeGen.h"

#ifdef TARGET_DIRECTX
#include <d3dcompiler.h>

#pragma comment(lib,"d3dcompiler.lib")

#endif

#define TEXTURE1D "Texture1D"
#define RW_TEXTURE1D "RWTexture1D"
#define TEXTURE2D "Texture2D"
#define RW_TEXTURE2D "RWTexture1D"
#define BUFFER "StructuredBuffer"
#define RW_BUFFER "RWStructuredBuffer"
#define SAMPLER2D "SamplerState"
#define SAMPLER3D "SamplerState"

namespace omegasl {

    class HLSLCodeGen final : public CodeGen {
        std::ostream & shaderOut;
        std::ofstream fileOut;
        std::ostringstream stringOut;
        HLSLCodeOpts & hlslCodeOpts;

        OmegaCommon::Map<OmegaCommon::String,OmegaCommon::String> generatedStructs;

    public:
        explicit HLSLCodeGen(CodeGenOpts &opts,HLSLCodeOpts & hlslCodeOpts): CodeGen(opts),shaderOut(fileOut),hlslCodeOpts(hlslCodeOpts){

        }
        explicit HLSLCodeGen(CodeGenOpts &opts,HLSLCodeOpts & hlslCodeOpts,std::ostringstream & out): CodeGen(opts),shaderOut(stringOut),hlslCodeOpts(hlslCodeOpts),stringOut(std::move(out)){

        }
        void generateExpr(ast::Expr *expr) override {
            switch (expr->type) {
                case BINARY_EXPR : {
                    auto _expr = (ast::BinaryExpr *)expr;
                    generateExpr(_expr->lhs);
                    shaderOut << " " << _expr->op << " ";
                    generateExpr(_expr->rhs);
                    break;
                }
                case LITERAL_EXPR : {
                    auto _expr = (ast::LiteralExpr *)expr;
                    if(_expr->isFloat()){
                         shaderOut << _expr->f_num.value();
                    }
                    else if(_expr->isDouble()){
                        shaderOut << _expr->d_num.value();
                    }
                    else if(_expr->isInt()){
                        shaderOut << _expr->i_num.value();
                    }
                    else if(_expr->isUint()) {
                        shaderOut << _expr->ui_num.value();
                    }
                    else if(_expr->isString()){
                        shaderOut << _expr->str.value();
                    }
                    break;
                }
                case ID_EXPR : {
                    auto _expr = (ast::IdExpr *)expr;
                    shaderOut << _expr->id << std::flush;
                    break;
                }
                case MEMBER_EXPR : {
                    auto _expr = (ast::MemberExpr *)expr;
                    generateExpr(_expr->lhs);
                    shaderOut << "." << _expr->rhs_id;
                    break;
                }
                case INDEX_EXPR : {
                    auto _expr = (ast::IndexExpr *)expr;
                    generateExpr(_expr->lhs);
                    shaderOut << "[";
                    generateExpr(_expr->idx_expr);
                    shaderOut << "]";
                    break;
                }
                case CALL_EXPR : {
                    auto _expr = (ast::CallExpr *)expr;
                    OmegaCommon::StrRef _id_expr = ((ast::IdExpr *)_expr->callee)->id;
                    bool generatedExprBody = false;
                    /// 1. If call expression is invoking standard method, generate HLSL std method impl.
                    if(_id_expr == BUILTIN_MAKE_FLOAT2){
                        shaderOut << "float2";
                    }
                    else if(_id_expr == BUILTIN_MAKE_FLOAT3){
                        shaderOut << "float3";
                    }
                    else if(_id_expr == BUILTIN_MAKE_FLOAT4){
                        shaderOut << "float4";
                    }
                    else if(_id_expr == BUILTIN_SAMPLE){
                        generatedExprBody = true;
                        /// Texture has instance method
                        generateExpr(_expr->args[1]);
                        shaderOut << ".Sample(";
                        generateExpr(_expr->args[0]);
                        shaderOut << ",";
                        generateExpr(_expr->args[2]);
                        shaderOut << ")";
                    }
                    else if(_id_expr == BUILTIN_WRITE){
                        /// Texture has method for accessing texel data.
                        generatedExprBody = true;
                        generateExpr(_expr->args[0]);
                        shaderOut << "[";
                        generateExpr(_expr->args[1]);
                        shaderOut << "]";
                    }
                    else {
                        shaderOut << _id_expr;
                    }

                    if(!generatedExprBody){
                        shaderOut << "(";
                        for(auto e_it = _expr->args.begin();e_it != _expr->args.end();e_it++){
                            if(e_it != _expr->args.begin()){
                                shaderOut << ",";
                            }
                            generateExpr(*e_it);
                        }
                        shaderOut << ")";
                    }
                    break;
                }
            }
        }
    private:
        unsigned level_count = 0;
    public:
        void generateBlock(ast::Block &block) override {
            shaderOut << "{" << std::endl;
            level_count += 1;
            for(auto s : block.body){
                for(unsigned i = 0;i < level_count;i++){
                    shaderOut << "  ";
                }
                if(s->type == VAR_DECL || s->type == RETURN_DECL){
                    generateDecl((ast::Decl *)s);
                }
                else {
                    generateExpr((ast::Expr *)s);
                }
                shaderOut << ";" << std::endl;
            }
            level_count -= 1;
            shaderOut << "}" << std::endl;
        }
        inline void writeAttribute(OmegaCommon::StrRef attributeName,std::ostream & out){
            if(attributeName == ATTRIBUTE_VERTEX_ID){
                out << "SV_VertexID";
            }
            else if(attributeName == ATTRIBUTE_POSITION){
                out << "SV_Position";
            }
            else if(attributeName == ATTRIBUTE_COLOR){
                out << "COLOR";
            }
            else if(attributeName == ATTRIBUTE_TEXCOORD){
                out << "TEXCOORD";
            }
            else if(attributeName == ATTRIBUTE_GLOBALTHREAD_ID){
                out << "SV_DispatchThreadID";
            }
            else if(attributeName == ATTRIBUTE_LOCALTHREAD_ID){
                out << "SV_GroupThreadID";
            }
            else if(attributeName == ATTRIBUTE_THREADGROUP_ID){
                out << "SV_GroupID";
            }
        }
        inline void writeTypeExpr(ast::TypeExpr *typeExpr,std::ostream & out){
            auto _ty = typeResolver->resolveTypeWithExpr(typeExpr);
            if(_ty == ast::builtins::float_type){
                out << "float";
            }
            else if(_ty == ast::builtins::float2_type){
                out << "float2";
            }
            else if(_ty == ast::builtins::float3_type){
                out << "float3";
            }
            else if(_ty == ast::builtins::float4_type){
                out << "float4";
            }
            else if(_ty == ast::builtins::int_type){
                out << "int";
            }
            else if(_ty == ast::builtins::uint_type){
                out << "uint";
            }
            else if(_ty == ast::builtins::uint2_type){
                out << "uint2";
            }
            else if(_ty == ast::builtins::uint3_type){
                out << "uint3";
            }
            else {
                out << _ty->name;
            }


            if(typeExpr->pointer){
                out << "*";
            }


        }
        void generateDecl(ast::Decl *decl) override {
            switch (decl->type) {
                case VAR_DECL : {
                    auto _decl = (ast::VarDecl *)decl;
                    writeTypeExpr(_decl->typeExpr,shaderOut);
                    shaderOut << " " << _decl->spec.name;
                    if(_decl->spec.initializer.has_value()){
                        shaderOut << " = ";
                        generateExpr(_decl->spec.initializer.value());
                    }
                    break;
                }
                case RETURN_DECL : {
                    auto _decl = (ast::ReturnDecl *)decl;
                    shaderOut << "return ";
                    generateExpr(_decl->expr);
                    break;
                }
                case STRUCT_DECL : {
                    auto _decl = (ast::StructDecl *)decl;
                    std::ostringstream out;
                    out << "struct " << _decl->name << "{" << std::endl;
                    for(auto & f : _decl->fields){
                        out << "    " << std::flush;
                        writeTypeExpr(f.typeExpr,out);
                        out << " " << f.name;
                        if(f.attributeName.has_value()){
                            out << ":";
                            writeAttribute(f.attributeName.value(),out);
                        }
                        out << ";" << std::endl;
                    }
                    out << "};" << std::endl;

                    generatedStructs.insert(std::make_pair(_decl->name,out.str()));

                    break;
                }
                case RESOURCE_DECL : {
                    resourceStore.add((ast::ResourceDecl *)decl);
                    break;
                }
                case SHADER_DECL : {
                    auto _decl = (ast::ShaderDecl *)decl;
                    if(opts.runtimeCompile) {
                        stringOut.str("");
                    }
                    else {
                        fileOut.open(OmegaCommon::FS::Path(opts.tempDir).append(_decl->name).concat(".hlsl").str());
                    }

                    omegasl_shader shaderDesc {};
                    /// 1. Write Structs for Shader
                    OmegaCommon::Vector<OmegaCommon::String> struct_names;
                    typeResolver->getStructsInFuncDecl(_decl,struct_names);

                    for(auto & s : struct_names){
                        shaderOut << generatedStructs[s] << std::endl;
                    }

                    shaderDesc.type =
                            _decl->shaderType == ast::ShaderDecl::Vertex? OMEGASL_SHADER_VERTEX :
                            _decl->shaderType == ast::ShaderDecl::Fragment? OMEGASL_SHADER_FRAGMENT :
                            OMEGASL_SHADER_COMPUTE;

//                    std::cout << "Shader Name:" << _decl->name << std::endl;

                    shaderDesc.name = new char[_decl->name.size() + 1];
                    std::copy(_decl->name.begin(),_decl->name.end(),(char *)shaderDesc.name);
                    ((char *)shaderDesc.name)[_decl->name.size()] = '\0';

                    OmegaCommon::Vector<omegasl_shader_layout_desc> shaderLayout;

                    /// 2. Write Resources for Shader
                    unsigned t_resource_count = 0,u_resource_count = 0,s_resource_count = 0;
                    for(auto & res : _decl->resourceMap){
                        auto res_desc = *(resourceStore.find(res.name));

                        omegasl_shader_layout_desc layoutDesc{};
                        layoutDesc.offset = 0;

                        auto _t = typeResolver->resolveTypeWithExpr(res_desc->typeExpr);

                        bool isTResource = false,isSResource = false;

                        layoutDesc.io_mode =
                                res.access == ast::ShaderDecl::ResourceMapDesc::In? OMEGASL_SHADER_DESC_IO_IN :
                                res.access == ast::ShaderDecl::ResourceMapDesc::Inout? OMEGASL_SHADER_DESC_IO_INOUT :
                                OMEGASL_SHADER_DESC_IO_OUT;

                        if(_t == ast::builtins::buffer_type){
                            layoutDesc.type = OMEGASL_SHADER_BUFFER_DESC;
                            isTResource = true;
                            if(res.access == ast::ShaderDecl::ResourceMapDesc::In){
                                shaderOut << BUFFER;
                            }
                            else {
                                shaderOut << RW_BUFFER;
                            }
                            shaderOut << "<";
                            writeTypeExpr(res_desc->typeExpr->args[0],shaderOut);
                            shaderOut << ">";

                        }
                        else if(_t == ast::builtins::texture1d_type){
                            layoutDesc.type = OMEGASL_SHADER_TEXTURE1D_DESC;
                            isTResource = true;
                            if(res.access == ast::ShaderDecl::ResourceMapDesc::In){
                                shaderOut << TEXTURE1D;
                            }
                            else {
                                shaderOut << RW_TEXTURE1D;
                            }
                        }
                        else if(_t == ast::builtins::texture2d_type){
                            layoutDesc.type = OMEGASL_SHADER_TEXTURE2D_DESC;
                            isTResource = true;
                            if(res.access == ast::ShaderDecl::ResourceMapDesc::In){
                                shaderOut << TEXTURE2D;
                            }
                            else {
                                shaderOut << RW_TEXTURE2D;
                            }
                        }
                        else if(_t == ast::builtins::sampler2d_type){
                            isSResource = true;
                            if(res_desc->isStatic) {
                                layoutDesc.type = OMEGASL_SHADER_STATIC_SAMPLER2D_DESC;
                            }
                            else {
                                layoutDesc.type = OMEGASL_SHADER_SAMPLER2D_DESC;
                            }
                            shaderOut << SAMPLER2D;
                        }

                        shaderOut << " " << res_desc->name;

                        auto convertAddressMode = [=](omegasl_shader_static_sampler_address_mode &mode,std::ostream & out){
                            switch (mode) {
                                case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_WRAP : {
                                    out << "Wrap";
                                    break;
                                }
                                case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRROR : {
                                    out << "MIRROR";
                                    break;
                                }
                                case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRRORWRAP : {
                                    out << "MirrorWrap";
                                    break;
                                }
                                case OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_CLAMPTOEDGE : {
                                    out << "ClampToEdge";
                                    break;
                                }
                            }
                        };

                        if(isSResource){
                            if(res_desc->isStatic){
                                /// Write Static Sampler Desc
#define INDENT "    "
                                layoutDesc.sampler_desc.filter = res_desc->staticSamplerDesc->filter;
                                layoutDesc.sampler_desc.u_address_mode = res_desc->staticSamplerDesc->uAddressMode;
                                layoutDesc.sampler_desc.v_address_mode = res_desc->staticSamplerDesc->vAddressMode;
                                layoutDesc.sampler_desc.w_address_mode = res_desc->staticSamplerDesc->wAddressMode;
                                layoutDesc.sampler_desc.max_anisotropy = res_desc->staticSamplerDesc->maxAnisotropy;
                                layoutDesc.location = res_desc->registerNumber;
                                shaderOut << ": register(s" << s_resource_count << ",space";
                                if(shaderDesc.type == OMEGASL_SHADER_FRAGMENT){
                                    shaderOut << "1";
                                }
                                else {
                                    shaderOut << "0";
                                }
                                shaderOut << ");" << std::endl;
                                s_resource_count += 1;
                            }
                        }

                        if(!res_desc->isStatic) {

                            shaderOut << ": register(";
                            layoutDesc.location = res_desc->registerNumber;
                            if (isTResource) {
                                shaderOut << "t" << t_resource_count;
                                layoutDesc.gpu_relative_loc = t_resource_count;
                                ++t_resource_count;
                            } else if (isSResource) {
                                shaderOut << "s" << s_resource_count;
                                layoutDesc.gpu_relative_loc = s_resource_count;
                                ++s_resource_count;
                            } else {
                                shaderOut << "u" << u_resource_count;
                                layoutDesc.gpu_relative_loc = u_resource_count;
                                ++u_resource_count;
                            }

                            shaderOut << ",";

                            /// Scope all relative resource bindings to relative shader.
                            if(shaderDesc.type == OMEGASL_SHADER_FRAGMENT){
                                shaderOut << "space1";
                            }
                            else {
                                shaderOut << "space0";
                            }
                            shaderOut << ");" << std::endl;
                        }
                        shaderLayout.push_back(layoutDesc);
                    }

                    shaderDesc.nLayout = shaderLayout.size();
                    shaderDesc.pLayout = new omegasl_shader_layout_desc[shaderLayout.size()];
                    std::copy(shaderLayout.begin(),shaderLayout.end(),shaderDesc.pLayout);

                    /// 3. Write Function Decl

                    if(_decl->shaderType == ast::ShaderDecl::Compute){
                        /// Write Threadgroup Size Desc.
                        shaderOut << "[numthreads("
                        << _decl->threadgroupDesc.x << ","
                        << _decl->threadgroupDesc.y << ","
                        << _decl->threadgroupDesc.z << ")]" << std::endl;
                    }

                    writeTypeExpr(_decl->returnType,shaderOut);
                    shaderOut << " " << _decl->name;
                    shaderOut << "(";
                    for(auto p_it = _decl->params.begin();p_it != _decl->params.end();p_it++){
                        if(p_it != _decl->params.begin()){
                            shaderOut << ",";
                        }

                        writeTypeExpr(p_it->typeExpr,shaderOut);
                        shaderOut << " " << p_it->name;
                        if(p_it->attributeName.has_value()){
                            if(p_it->attributeName.value() == ATTRIBUTE_VERTEX_ID){
                                shaderDesc.vertexShaderInputDesc.useVertexID = true;
                            }
                            else if(p_it->attributeName.value() == ATTRIBUTE_GLOBALTHREAD_ID){
                                shaderDesc.computeShaderParamsDesc.useGlobalThreadID = true;
                            }
                            else if(p_it->attributeName.value() == ATTRIBUTE_LOCALTHREAD_ID){
                                shaderDesc.computeShaderParamsDesc.useLocalThreadID = true;
                            }
                            else if(p_it->attributeName.value() == ATTRIBUTE_THREADGROUP_ID){
                                shaderDesc.computeShaderParamsDesc.useThreadGroupID = true;
                            }
                            shaderOut << ":";
                            writeAttribute(p_it->attributeName.value(),shaderOut);
                        }
                    }
                    shaderOut << ")";
                    if(_decl->shaderType == ast::ShaderDecl::Fragment){
                        shaderOut << " : SV_TARGET";
                    }
                    generateBlock(*_decl->block);

                    OmegaCommon::String object_file;
                    if(!opts.runtimeCompile) {
                        object_file = OmegaCommon::FS::Path(opts.tempDir).append(_decl->name).concat(".cso").str();
                    }
                    else {
                        object_file = _decl->name;
                    }
                    if(!opts.runtimeCompile) {
                        fileOut.close();
                    }

                    shaderMap.insert(std::make_pair(object_file,shaderDesc));
                    break;
                }
            }
        }
        void compileShader(ast::ShaderDecl::Type type, const OmegaCommon::StrRef &name, const OmegaCommon::FS::Path &path, const OmegaCommon::FS::Path &outputPath) override {
            std::ostringstream out;
            out << " -nologo -T";
            if(type == ast::ShaderDecl::Vertex){
                out << "vs_5_0";
            }
            else if(type == ast::ShaderDecl::Fragment){
                out << "ps_5_0";
            }
            else if(type == ast::ShaderDecl::Compute){
                out << "cs_5_0";
            }
            out << " -E" << name.data() << " -Fo " << OmegaCommon::FS::Path(outputPath).append(name).concat(".cso").str();
            out << " " << OmegaCommon::FS::Path(path).append(name).concat(".hlsl").str() << " /Zi";

            auto dxc_process = OmegaCommon::ChildProcess::OpenWithStdoutPipe(hlslCodeOpts.dxc_cmd,out.str().c_str());
            auto res = dxc_process.wait();
        }

        void compileShaderOnRuntime(ast::ShaderDecl::Type type, const OmegaCommon::StrRef &name) override {
#ifdef TARGET_DIRECTX
            auto source = stringOut.str();
            ID3DBlob *blob;
            OmegaCommon::String target;
            if(type == ast::ShaderDecl::Vertex){
                target = "vs_5_1";
            }
            else if(type == ast::ShaderDecl::Fragment){
                target = "ps_5_1";
            }
            else if(type == ast::ShaderDecl::Compute){
                target = "cs_5_1";
            }

            ID3DBlob *errorBlob;

            D3DCompile(source.data(),
                       source.size(),
                       name.data(),
                       nullptr,
                       D3D_COMPILE_STANDARD_FILE_INCLUDE,
                       name.data(),
                       target.data(),
                       D3DCOMPILE_DEBUG,
                       NULL
                       ,&blob,
                       &errorBlob);

            if(errorBlob != nullptr){
                std::cout << "OMEGASL COMPILE ERROR: D3D ERROR:" << (char *)errorBlob->GetBufferPointer() << std::endl;

//                exit(1);
            }

            auto & shaderEntry = shaderMap[name.data()];
            shaderEntry.data = blob->GetBufferPointer();
            shaderEntry.dataSize = blob->GetBufferSize();
#endif
        }
    };

    std::shared_ptr<CodeGen> HLSLCodeGenMake(CodeGenOpts &opts,HLSLCodeOpts & hlslCodeOpts){
        return std::make_shared<HLSLCodeGen>(opts,hlslCodeOpts);
    }

    std::shared_ptr<CodeGen> HLSLCodeGenMakeRuntime(CodeGenOpts &opts,HLSLCodeOpts & hlslCodeOpts,std::ostringstream & out){
        return std::make_shared<HLSLCodeGen>(opts,hlslCodeOpts,out);
    }




}