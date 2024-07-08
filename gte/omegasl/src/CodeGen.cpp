#include "CodeGen.h"

namespace omegasl {
    void CodeGen::generateInterfaceAndCompileShader(ast::Decl *decl) {
//        if(decl->type == STRUCT_DECL){
//            interfaceGen->generateStruct((ast::StructDecl *)decl);
//        }
        if(decl->type == SHADER_DECL){
            auto _decl = (ast::ShaderDecl *)decl;
            if(opts.runtimeCompile){
                compileShaderOnRuntime(_decl->shaderType,_decl->name);
            }
            else {
                compileShader(_decl->shaderType,_decl->name,opts.tempDir,opts.tempDir);
            }
        }
    }
}