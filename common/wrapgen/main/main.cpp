#include "omega-common/common.h"
#include "../parser.h"
#include <iostream>
#include <filesystem>
#include <memory>

void printHelp(){
    std::cout << 
    R"(
    Omega Wrapper Generator
    =======================
    Usage : omega-wrapgen [options] <input_file>
    
    Options:
    
       Args:
           --help, -h        -> Display this info
           --output-dir, -o  -> Output dir of generated wrapper files
       Modes:
           --cc     -> Generate C Code
           --python -> Generate Python Code
           --go     -> Generate Go Code
           --java   -> Generate Java Code
           --swift  -> Generate Swift Code
           --rust   -> Generate Rust Code)" << std::endl;
};

int main(int argc,char *argv[]){
    enum class Mode {
        C,
        Python,
        Go,
        Java,
        Swift,
        Rust
    };

    if(argc < 2){
        printHelp();
        return 1;
    }

    OmegaCommon::String src_file;
    OmegaCommon::String output_dir = ".";
    Mode mode = Mode::C;

    for(int i = 1; i < argc; ++i){
        OmegaCommon::StrRef arg(argv[i]);
        if(arg == "--help" || arg == "-h"){
            printHelp();
            return 0;
        }
        else if(arg == "-o" || arg == "--output-dir"){
            if((i + 1) >= argc){
                std::cerr << "ERROR: missing output directory after " << arg.data() << std::endl;
                return 1;
            }
            output_dir = argv[++i];
        }
        else if(arg == "--cc"){
            mode = Mode::C;
        }
        else if(arg == "--python"){
            mode = Mode::Python;
        }
        else if(arg == "--go"){
            mode = Mode::Go;
        }
        else if(arg == "--java"){
            mode = Mode::Java;
        }
        else if(arg == "--swift"){
            mode = Mode::Swift;
        }
        else if(arg == "--rust"){
            mode = Mode::Rust;
        }
        else if(arg.data()[0] == '-'){
            std::cerr << "ERROR: unknown option " << arg.data() << std::endl;
            return 1;
        }
        else {
            if(!src_file.empty()){
                std::cerr << "ERROR: multiple input files provided: " << src_file << " and " << arg.data() << std::endl;
                return 1;
            }
            src_file = arg.data();
        }
    }

    if(src_file.empty()){
        std::cerr << "ERROR: missing input .owrap file" << std::endl;
        return 1;
    }

    std::ifstream in(src_file);
    if(!in.is_open()){
        std::cerr << "ERROR: failed to open input file " << src_file << std::endl;
        return 1;
    }

    if(!std::filesystem::exists(output_dir)){
        std::filesystem::create_directory(output_dir);
    }

    auto module_name = OmegaCommon::FS::Path(src_file).filename();
    if(module_name.size() > 6 && module_name.substr(module_name.size() - 6) == ".owrap"){
        module_name = module_name.substr(0,module_name.size() - 6);
    }
    OmegaWrapGen::GenContext gen_ctxt {module_name,output_dir};

    std::unique_ptr<OmegaWrapGen::Gen> generator;
    OmegaWrapGen::CGenSettings c_settings {OmegaWrapGen::CGenSettings::Retain};
    OmegaWrapGen::PythonGenSettings python_settings {false};
    OmegaWrapGen::GoGenSettings go_settings {};
    OmegaWrapGen::JavaGenSettings java_settings {};
    OmegaWrapGen::SwiftGenSettings swift_settings {};
    OmegaWrapGen::RustGenSettings rust_settings {};

    switch(mode){
        case Mode::C: {
            generator.reset(OmegaWrapGen::Gen::CreateCGen(c_settings));
            break;
        }
        case Mode::Python: {
            generator.reset(OmegaWrapGen::Gen::CreatePythonGen(python_settings));
            break;
        }
        case Mode::Go: {
            generator.reset(OmegaWrapGen::Gen::CreateGoGen(go_settings));
            break;
        }
        case Mode::Java: {
            generator.reset(OmegaWrapGen::Gen::CreateJavaGen(java_settings));
            break;
        }
        case Mode::Swift: {
            generator.reset(OmegaWrapGen::Gen::CreateSwiftGen(swift_settings));
            break;
        }
        case Mode::Rust: {
            generator.reset(OmegaWrapGen::Gen::CreateRustGen(rust_settings));
            break;
        }
    }

    generator->setContext(gen_ctxt);

    OmegaWrapGen::Parser parser(generator.get());
    parser.setInputStream(&in);
    parser.beginParse();
    parser.finish();

    generator->finish();

    if(parser.hasErrors()){
        return 1;
    }

    return 0;
};
