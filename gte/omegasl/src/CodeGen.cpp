#include "CodeGen.h"

namespace omegasl {
    bool CodeGen::generateInterfaceAndCompileShader(ast::Decl *decl) {
        if(decl->type == SHADER_DECL){
            auto _decl = (ast::ShaderDecl *)decl;
            /// If the backend already refused to emit this stage (e.g.
            /// Metal hull/domain), it has set hasFatalErrors and skipped
            /// writing the source file. Don't invoke the downstream
            /// compiler — it would just print a confusing
            /// "no such file" error on top of our diagnostic.
            if(hasFatalErrors){
                return true;
            }
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