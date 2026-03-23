#include "omegasl.h"
#include "../omegasl/src/CodeGen.h"
#include "../omegasl/src/Parser.h"

#include "OmegaGTE.h"
#include <iostream>
#include <stdexcept>

#ifdef RUNTIME_SHADER_COMP_SUPPORT


struct SourceImpl : public OmegaSLCompiler::Source {
    bool file;
    std::ifstream in_file;
    std::istringstream in;
    explicit SourceImpl(OmegaCommon::FS::Path & path):file(true),in_file(path.absPath(),std::ios::in){

    };
    explicit SourceImpl(OmegaCommon::String & buffer):file(false),in(buffer){

    }
};

std::shared_ptr<OmegaSLCompiler::Source> OmegaSLCompiler::Source::fromFile(OmegaCommon::FS::Path path) {
    return std::shared_ptr<SourceImpl>(new SourceImpl(path));
}

std::shared_ptr<OmegaSLCompiler::Source> OmegaSLCompiler::Source::fromString(OmegaCommon::String &buffer) {
    return std::shared_ptr<SourceImpl>(new SourceImpl(buffer));
}


class OmegaSLCompilerImpl : public OmegaSLCompiler {
    SharedHandle<OmegaGTE::GTEDevice> device;
    std::shared_ptr<omegasl::CodeGen> gen;
    std::shared_ptr<omegasl::Parser> parser;
    omegasl::CodeGenOpts genOpts;
    std::ostringstream sourceBuf;
    omegasl::DiagnosticEngine diagnostics;
#if defined(TARGET_METAL)
    omegasl::MetalCodeOpts metalCodeOpts;
#endif
#if defined(TARGET_VULKAN)
    omegasl::GLSLCodeOpts glslCodeOpts;
#endif
public:
    explicit OmegaSLCompilerImpl(SharedHandle<OmegaGTE::GTEDevice> & device):device(device), genOpts({false,true,}){
        omegasl::ast::builtins::Initialize();
#if defined(TARGET_DIRECTX)
        omegasl::HLSLCodeOpts hlslCodeOpts {""};
        gen = omegasl::HLSLCodeGenMakeRuntime(genOpts,hlslCodeOpts,sourceBuf);
#elif defined(TARGET_METAL)
        metalCodeOpts = omegasl::MetalCodeOpts {"",const_cast<void *>(device->native())};
        gen = omegasl::MetalCodeGenMakeRuntime(genOpts,metalCodeOpts,sourceBuf);
#elif defined(TARGET_VULKAN)
        glslCodeOpts.glslc_cmd = "glslc";
        gen = omegasl::GLSLCodeGenMakeRuntime(genOpts,glslCodeOpts,sourceBuf);
#endif
        if(gen == nullptr){
            throw std::runtime_error("OmegaSL runtime compiler backend is unavailable for this target.");
        }
        parser = std::make_shared<omegasl::Parser>(gen);

    }
    std::shared_ptr<omegasl_shader_lib> compile(std::initializer_list<std::shared_ptr<Source>> sources) override {
       for(auto & s : sources){
           auto source = (SourceImpl *)s.get();
           std::istream *in = nullptr;
           if(source->file){
                in = &source->in_file;
           }
           else {
              in = &source->in;
           }
           omegasl::ParseContext context {*in, nullptr, &diagnostics};
           parser->parseContext(context);
           if(diagnostics.hasErrors()){
               std::cerr << "[OmegaSL Runtime] Shader compilation produced " << diagnostics.getErrorCount() << " error(s):" << std::endl;
               diagnostics.report(std::cerr);
           }
       }
       std::cout << "[OmegaSL Runtime] Generated HLSL:\n" << sourceBuf.str() << std::endl;
       auto res = gen->getLibrary("RUNTIME");
       gen->resetShaderMap();
       return res;
    }

    ~OmegaSLCompilerImpl() {
        omegasl::ast::builtins::Cleanup();
    }
};

std::shared_ptr<OmegaSLCompiler> OmegaSLCompiler::Create(SharedHandle<OmegaGTE::GTEDevice> & device) {
    return std::make_shared<OmegaSLCompilerImpl>(device);
}



#endif
