#include "omegasl.h"
#include "../omegasl/src/CodeGen.h"
#include "../omegasl/src/Parser.h"

#include "OmegaGTE.h"

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
    OmegaGTE::SharedHandle<OmegaGTE::GTEDevice> device;
    std::shared_ptr<omegasl::CodeGen> gen;
    std::shared_ptr<omegasl::Parser> parser;
    omegasl::CodeGenOpts genOpts;
    std::ostringstream sourceBuf;
#if defined(TARGET_METAL)
    omegasl::MetalCodeOpts metalCodeOpts;
#endif
public:
    explicit OmegaSLCompilerImpl(OmegaGTE::SharedHandle<OmegaGTE::GTEDevice> & device):device(device), genOpts({false,true,}){
        omegasl::ast::builtins::Initialize();
#if defined(TARGET_DIRECTX)
        omegasl::HLSLCodeOpts hlslCodeOpts {""};
        gen = omegasl::HLSLCodeGenMakeRuntime(genOpts,hlslCodeOpts,sourceBuf);
#elif defined(TARGET_METAL)
        metalCodeOpts = omegasl::MetalCodeOpts {"",const_cast<void *>(device->native())};
        gen = omegasl::MetalCodeGenMakeRuntime(genOpts,metalCodeOpts,sourceBuf);
#endif
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
           omegasl::ParseContext context {*in};
           parser->parseContext(context);
       }
       auto res = gen->getLibrary("RUNTIME");
       gen->resetShaderMap();
       return res;
    }

    ~OmegaSLCompilerImpl() {
        omegasl::ast::builtins::Cleanup();
    }
};

std::shared_ptr<OmegaSLCompiler> OmegaSLCompiler::Create(OmegaGTE::SharedHandle<OmegaGTE::GTEDevice> & device) {
    return std::make_shared<OmegaSLCompilerImpl>(device);
}



#endif