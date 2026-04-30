#include <omega-common/common.h>

#include "Parser.h"
#include "CodeGen.h"
#include "Error.h"
#include "Preprocessor.h"

#include <iostream>
#include <sstream>
#include <fstream>

inline void help(){
    std::cout <<
    R"(Usage: omegaslc [options] [required] input-file

Required:
    --temp-dir, -t                  --> Set the temp file output dir (For byproducts of compiling the lib)
    --output,-o                     --> Set the output *.omegasllib (not required with --emit-source-only).
Options:

    --help ,    -h                  --> Show this message.
    --tokens-only                   --> Show tokens of all input files.
    --interface-only                --> Emit interface of all input files.
    --emit-source-only, -S          --> Transpile to the target language and stop. Writes generated
                                        source files to --temp-dir; does not invoke dxc/metal/glslc
                                        and does not link a .omegasllib. Lets you cross-target HLSL
                                        or GLSL from a non-Windows / non-Linux host for source-level
                                        debugging. Runtime correctness still has to be exercised on
                                        the matching platform.


    --hlsl                          --> Generate HLSL code.
    --metal                         --> Generate Metal Shading Language code.
    --glsl                          --> Generate GLSL code.

HLSL Options:
    --dxc                           --> Path to dxc compiler

Metal Options:
    --target-arch=[x86_64,aarch64]  --> Select the target architecture to compile the MSL to.

GLSL Options:
    --glslc                         --> Path to glslc compiler
    )" << std::endl;
}

enum class GenMode : int {
    glsl,
    hlsl,
    metal,
    unknown
};

GenMode defaultGenModeForHost(){
    #ifdef _WIN32
        return GenMode::hlsl;
    #elif defined(__APPLE__)
        return GenMode::metal;
    #else
        return GenMode::glsl;
    #endif
}

int main(int argc,char *argv[]){

    omegasl::ast::builtins::Initialize();

    bool tokenize = false;
    bool interfaceOnly = false;
    bool emitSourceOnly = false;

    GenMode genMode = defaultGenModeForHost();
    const char * outputLibn = nullptr,*tempDir = nullptr;

    const char *glslc_cmd = nullptr,*dxc_cmd = nullptr;

    OmegaCommon::StrRef inputFile = argv[argc - 1];

    /// First pass: pick up `--emit-source-only` so the genMode-platform
    /// gates know whether to allow cross-target transpilation. Keeps the
    /// per-platform `dxc` / `metal` / `glslc` requirement intact for the
    /// default flow, while letting `-S` (source-only) work on any host.
    for(unsigned i = 1;i < argc;i++){
        OmegaCommon::StrRef arg{argv[i]};
        if(arg == "--emit-source-only" || arg == "-S"){
            emitSourceOnly = true;
        }
    }

    for(unsigned i = 1;i < argc;i++){
        OmegaCommon::StrRef arg{argv[i]};
        if(arg == "--help" || arg == "-h"){
            help();
            return 0;
        }
        else if(arg == "--metal"){
#ifdef TARGET_METAL
            genMode = GenMode::metal;
#else
            if(!emitSourceOnly){
                std::cerr << "Metal code can only be compiled on an Apple Device. "
                             "Use --emit-source-only to transpile without compiling." << std::endl;
                return 1;
            }
            genMode = GenMode::metal;
#endif
        }
        else if(arg == "--hlsl"){
#ifdef TARGET_DIRECTX
            genMode = GenMode::hlsl;
#else
            if(!emitSourceOnly){
                std::cerr << "HLSL code can only be compiled on a Windows Device. "
                             "Use --emit-source-only to transpile without compiling." << std::endl;
                return 1;
            }
            genMode = GenMode::hlsl;
#endif
        }
        else if(arg == "--glsl"){
#ifdef TARGET_VULKAN
            genMode = GenMode::glsl;
#else
            if(!emitSourceOnly){
                std::cerr << "GLSL code can only be compiled on a Linux Device. "
                             "Use --emit-source-only to transpile without compiling." << std::endl;
                return 1;
            }
            genMode = GenMode::glsl;
#endif
        }
        else if(arg == "--tokens-only"){
            tokenize = true;
        }
        else if(arg == "--interface-only"){
            interfaceOnly = true;
        }
        else if(arg == "--emit-source-only" || arg == "-S"){
            /// Already handled in the first pass.
        }
        else if(arg == "--output" || arg == "-o"){
            outputLibn = argv[++i];
        }
        else if(arg == "--temp-dir" || arg == "-t"){
            tempDir = argv[++i];
        }

        if(genMode == GenMode::metal){

        }
        else if(genMode == GenMode::glsl){
            if(arg == "--glslc"){
                glslc_cmd = argv[++i];
            }
        }
        else if(genMode == GenMode::hlsl){
            if(arg == "--dxc"){
                dxc_cmd = argv[++i];
            }
        }
    }


    if(tempDir == nullptr){
        std::cout << "Temp Directory is not set" << std::endl;
        exit(1);
    }

    /// `-o` is only required when we link a final `.omegasllib`; in
    /// `--emit-source-only` mode there's nothing to link, so accept its
    /// absence and pass an empty string into CodeGenOpts.
    if(outputLibn == nullptr && !emitSourceOnly){
        std::cout << "Output Lib is not set" << std::endl;
        exit(1);
    }

    if(outputLibn != nullptr){
        OmegaCommon::FS::Path outputLib(outputLibn);
        auto outputPath = OmegaCommon::FS::Path(OmegaCommon::FS::Path(outputLib).dir());
        if(!OmegaCommon::FS::exists(outputPath)){
            OmegaCommon::FS::createDirectory(outputPath);
        }
    }

    OmegaCommon::FS::Path tempPath(tempDir);

    if(!OmegaCommon::FS::exists(tempPath)){
        OmegaCommon::FS::createDirectory(tempPath);
    };

    if(!OmegaCommon::FS::exists(inputFile)){
        std::cout << "File `" << inputFile << "` does not exist." << std::endl;
        return 1;
    }

    auto input_file_path = OmegaCommon::FS::Path(inputFile);

    std::ifstream fileIn(inputFile.data(), std::ios::in);
    if(!fileIn){
        std::cerr << "error: cannot open input file " << inputFile << std::endl;
        return 1;
    }
    std::stringstream buffer;
    buffer << fileIn.rdbuf();
    fileIn.close();
    std::string sourceContent = buffer.str();

    omegasl::Preprocessor preprocessor;
    std::string processedSource = preprocessor.process(sourceContent, input_file_path.dir());

    omegasl::SourceFile sourceFile;
    sourceFile.setContent(processedSource);
    sourceFile.buildLinePosMap();

    omegasl::DiagnosticEngine diagnostics;
    diagnostics.setSourceFile(&sourceFile);

    std::istringstream in(processedSource);

    if(tokenize){
        auto lexer = OmegaCommon::makeARCAny<omegasl::Lexer>();
        lexer->setInputStream(&in);
        omegasl::Tok t;
        while((t = lexer->nextTok()).type != TOK_EOF){
            std::cout << "Tok {type:" << std::hex << t.type << std::dec << ", str:`" << t.str << "`}" << std::endl;
        }
        lexer->finishTokenizeFromStream();
        return 0;
    }


    std::shared_ptr<omegasl::CodeGen> codeGen;

    omegasl::CodeGenOpts codeGenOpts {
        interfaceOnly,
        false,
        emitSourceOnly,
        outputLibn != nullptr ? OmegaCommon::StrRef(outputLibn) : OmegaCommon::StrRef(""),
        tempDir
    };
    omegasl::MetalCodeOpts metalCodeOpts {};
    omegasl::GLSLCodeOpts glslCodeOpts {};
    omegasl::HLSLCodeOpts hlslCodeOpts {};

    if(genMode == GenMode::hlsl){
#ifdef TARGET_DIRECTX
        hlslCodeOpts.dxc_cmd = "dxc";
        if(dxc_cmd != nullptr){
            hlslCodeOpts.dxc_cmd = dxc_cmd;
        }
#endif
        codeGen = omegasl::CodeGenMake(codeGenOpts, std::make_unique<omegasl::HLSLTarget>(hlslCodeOpts));
    }
    else if(genMode == GenMode::metal){
#ifdef TARGET_METAL
        metalCodeOpts.mtl_device = nullptr;
        metalCodeOpts.metal_cmd = "xcrun -sdk macosx metal";
#endif
        codeGen = omegasl::CodeGenMake(codeGenOpts, std::make_unique<omegasl::MSLTarget>(metalCodeOpts));
    }
    else {
        #ifdef TARGET_VULKAN
        #ifdef OMEGASL_DEFAULT_GLSLC
        glslCodeOpts.glslc_cmd = OMEGASL_DEFAULT_GLSLC;
        #else
        glslCodeOpts.glslc_cmd = "glslc";
        #endif
        if(glslc_cmd != nullptr){
            glslCodeOpts.glslc_cmd = glslc_cmd;
        }
        #endif
        codeGen = omegasl::CodeGenMake(codeGenOpts, std::make_unique<omegasl::GLSLTarget>(glslCodeOpts));
    }


    omegasl::Parser parser(codeGen);
    omegasl::ParseContext parseCtx{ in };
    parseCtx.sourceFile = &sourceFile;
    parseCtx.diagnostics = &diagnostics;
    parser.parseContext(parseCtx);

    if(diagnostics.hasErrors()){
        diagnostics.report(std::cerr);
        omegasl::ast::builtins::Cleanup();
        return 1;
    }

    /// A backend can refuse to emit a stage it doesn't support yet
    /// (e.g. Metal hull/domain — see OmegaSL-Reference.md bug 3).
    /// In that case the backend has already printed a diagnostic;
    /// exit nonzero without attempting to link a partial library.
    if(codeGen->hasFatalErrors){
        omegasl::ast::builtins::Cleanup();
        return 1;
    }

    /// `--emit-source-only` stops after the codegen pass writes transpiled
    /// shaders to tempDir; nothing to link.
    if(!emitSourceOnly){
        if(!codeGen->linkShaderObjects()){
            omegasl::ast::builtins::Cleanup();
            return 1;
        }
    }

    omegasl::ast::builtins::Cleanup();

    return 0;
};
