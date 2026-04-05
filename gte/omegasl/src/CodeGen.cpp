#include "CodeGen.h"

namespace omegasl {
    bool CodeGen::generateInterfaceAndCompileShader(ast::Decl *decl) {
        if(decl->type == SHADER_DECL){
            auto _decl = (ast::ShaderDecl *)decl;
            if(opts.runtimeCompile){
                compileShaderOnRuntime(_decl->shaderType,_decl->name);
            }
            else {
                if(!compileShader(_decl->shaderType,_decl->name,opts.tempDir,opts.tempDir)){
                    return false;
                }
            }
        }
        return true;
    }
}