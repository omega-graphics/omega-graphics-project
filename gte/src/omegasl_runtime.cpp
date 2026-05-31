#include "omegasl.h"
#include "../omegasl/src/CodeGen.h"
#include "../omegasl/src/Parser.h"
#include "../omegasl/src/Preprocessor.h"

#include "omegaGTE/GTEDevice.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

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
        gen = omegasl::CodeGenMakeRuntime(genOpts, std::make_unique<omegasl::HLSLTarget>(hlslCodeOpts), sourceBuf);
#elif defined(TARGET_METAL)
        metalCodeOpts = omegasl::MetalCodeOpts {"",const_cast<void *>(device->native())};
        gen = omegasl::CodeGenMakeRuntime(genOpts, std::make_unique<omegasl::MSLTarget>(metalCodeOpts), sourceBuf);
#elif defined(TARGET_VULKAN)
        glslCodeOpts.glslc_cmd = "glslc";
        gen = omegasl::CodeGenMakeRuntime(genOpts, std::make_unique<omegasl::GLSLTarget>(glslCodeOpts), sourceBuf);
#endif
        if(gen == nullptr){
            throw std::runtime_error("OmegaSL runtime compiler backend is unavailable for this target.");
        }
        parser = std::make_shared<omegasl::Parser>(gen);

    }
    std::shared_ptr<omegasl_shader_lib> compile(std::initializer_list<std::shared_ptr<Source>> sources) override {
       /// Pick the PPBackend that matches the codegen target stitched
       /// together in the ctor. Used per-source to predefine the right
       /// `OMEGASL_BACKEND_<X>` + `OMEGASL_FEATURE_<NAME>` macros so
       /// `#requires(...)` and `#if defined(...)` resolve the same way
       /// they do under offline `omegaslc`.
#if defined(TARGET_DIRECTX)
       const omegasl::PPBackend ppBackend = omegasl::PPBackend::HLSL;
#elif defined(TARGET_METAL)
       const omegasl::PPBackend ppBackend = omegasl::PPBackend::MSL;
#else
       const omegasl::PPBackend ppBackend = omegasl::PPBackend::GLSL;
#endif

       for(auto & s : sources){
           auto source = (SourceImpl *)s.get();
           std::istream *in = nullptr;
           if(source->file){
                in = &source->in_file;
           }
           else {
              in = &source->in;
           }

           /// Run the OmegaSL preprocessor on the raw source so `#requires`,
           /// `#define`, and `#if defined(...)` work at runtime the same
           /// way they do offline. `#include` is rejected because a
           /// runtime source string has no file-system context for
           /// resolving include paths reliably — callers must concatenate
           /// dependencies into the source string instead. A fresh
           /// Preprocessor per source keeps the macro / required-features
           /// state isolated, matching the offline `main.cpp` pattern.
           std::ostringstream slurp;
           slurp << in->rdbuf();
           std::string rawSource = slurp.str();

           omegasl::Preprocessor preprocessor;
           preprocessor.setBackend(ppBackend);
           preprocessor.setRejectIncludes(true);
           std::string processedSource = preprocessor.process(rawSource, /*currentPath=*/"");
           if (preprocessor.hasErrors()) {
               std::cerr << "[OmegaSL Runtime] Preprocessor errors; skipping parse for this source." << std::endl;
               continue;
           }

           /// Hand the file-scope `#requires` bitfield to CodeGen before
           /// parsing the source — `SHADER_DECL` reads
           /// `fileRequiredFeatures` while it builds each
           /// `omegasl_shader` record. Mirrors `main.cpp:296` byte-for-byte.
           gen->setRequiredFeatures(preprocessor.requiredFeatures(),
                                    preprocessor.unsatisfiedRequiredFeatures());

           std::istringstream processedStream(processedSource);
           omegasl::ParseContext context {processedStream, nullptr, &diagnostics};
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

