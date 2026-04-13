#include "omega-common/common.h"
#include "omega-common/cli.h"
#include "../parser.h"
#include <iostream>
#include <filesystem>
#include <memory>

int main(int argc,char *argv[]){
    enum class Mode {
        C,
        Python,
        Go,
        Java,
        Swift,
        Rust
    };

    auto configure_cli =
        [](OmegaCommon::Argv::Parser & cli,
           bool & help_flag,
           OmegaCommon::String & output_dir,
           bool & mode_cc,
           bool & mode_python,
           bool & mode_go,
           bool & mode_java,
           bool & mode_swift,
           bool & mode_rust,
           OmegaCommon::String & input_file) {
            cli.setDescription("Omega Wrapper Generator");
            cli.setUsage("[options] <input_file>");
            cli.addFlag(help_flag,"help","h","Display this info");
            cli.addOption(output_dir,"output-dir","o","dir","Output dir of generated wrapper files");
            cli.addFlag(mode_cc,"cc",{},"Generate C code");
            cli.addFlag(mode_python,"python",{},"Generate Python code");
            cli.addFlag(mode_go,"go",{},"Generate Go code");
            cli.addFlag(mode_java,"java",{},"Generate Java code");
            cli.addFlag(mode_swift,"swift",{},"Generate Swift code");
            cli.addFlag(mode_rust,"rust",{},"Generate Rust code");
            cli.addPositional(input_file,"input_file","Input .owrap file");
        };

    for(int i = 1; i < argc; ++i){
        OmegaCommon::StrRef arg(argv[i]);
        if(arg == "--help" || arg == "-h"){
            OmegaCommon::Argv::Parser help_cli("omega-wrapgen");
            help_cli.setDescription("Omega Wrapper Generator");
            bool help_flag = false;
            OmegaCommon::String help_output_dir;
            bool help_mode_cc = false;
            bool help_mode_python = false;
            bool help_mode_go = false;
            bool help_mode_java = false;
            bool help_mode_swift = false;
            bool help_mode_rust = false;
            OmegaCommon::String help_input_file;

            configure_cli(help_cli,help_flag,help_output_dir,help_mode_cc,help_mode_python,help_mode_go,help_mode_java,help_mode_swift,help_mode_rust,help_input_file);
            help_cli.printHelp(std::cout);
            return 0;
        }
    }

    OmegaCommon::String src_file;
    OmegaCommon::String output_dir = ".";
    bool show_help = false;
    bool mode_cc = false;
    bool mode_python = false;
    bool mode_go = false;
    bool mode_java = false;
    bool mode_swift = false;
    bool mode_rust = false;

    OmegaCommon::Argv::Parser cli("omega-wrapgen");
    configure_cli(cli,show_help,output_dir,mode_cc,mode_python,mode_go,mode_java,mode_swift,mode_rust,src_file);

    OmegaCommon::Argv::ParseError parse_error;
    if(!cli.parse(argc,argv,&parse_error)){
        std::cerr << "ERROR: " << parse_error.toString() << std::endl;
        return 1;
    }

    if(src_file.empty()){
        std::cerr << "ERROR: missing input .owrap file" << std::endl;
        cli.printHelp(std::cerr);
        return 1;
    }

    (void)mode_cc;
    (void)mode_python;
    (void)mode_go;
    (void)mode_java;
    (void)mode_swift;
    (void)mode_rust;

    Mode mode = Mode::C;
    for(int i = 1; i < argc; ++i){
        OmegaCommon::StrRef arg(argv[i]);
        if(arg == "-o" || arg == "--output-dir"){
            ++i;
            continue;
        }
        if(OmegaCommon::startsWith(arg,"--output-dir=")){
            continue;
        }
        if(arg == "--cc"){
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
