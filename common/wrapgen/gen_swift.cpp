#include "wrapper_gen.h"
#include <fstream>

namespace OmegaWrapGen {
    class SwiftGen final : public Gen {
        SwiftGenSettings & settings;
        GenContext *context;
        std::ofstream out;
        unsigned decl_count = 0;
    public:
        explicit SwiftGen(SwiftGenSettings & settings):settings(settings),context(nullptr){

        }

        GenContext & getContext() override {
            return *context;
        }

        void setContext(GenContext &ctxt) override {
            context = &ctxt;
            auto output_file = std::string(ctxt.output_dir).append("/").append(ctxt.name).append(".swift");
            out.open(output_file,std::ios::out);
            out << "// " << COMMENT_HEADER << "\n";
            out << "enum " << ctxt.name << "Wrapper {\n";
        }

        void consumeDecl(DeclNode *node) override {
            (void)node;
            ++decl_count;
        }

        void finish() override {
            if(out.is_open()){
                out << "    static let declCount = " << decl_count << "\n";
                out << "}\n";
                out.close();
            }
        }
    };

    class RustGen final : public Gen {
        RustGenSettings & settings;
        GenContext *context;
        std::ofstream out;
        unsigned decl_count = 0;
    public:
        explicit RustGen(RustGenSettings & settings):settings(settings),context(nullptr){

        }

        GenContext & getContext() override {
            return *context;
        }

        void setContext(GenContext &ctxt) override {
            context = &ctxt;
            auto output_file = std::string(ctxt.output_dir).append("/").append(ctxt.name).append(".rs");
            out.open(output_file,std::ios::out);
            out << "// " << COMMENT_HEADER << "\n";
        }

        void consumeDecl(DeclNode *node) override {
            (void)node;
            ++decl_count;
        }

        void finish() override {
            if(out.is_open()){
                out << "pub const DECL_COUNT: usize = " << decl_count << ";\n";
                out.close();
            }
        }
    };

    Gen *Gen::CreateSwiftGen(SwiftGenSettings & settings){
        return new SwiftGen(settings);
    }

    Gen *Gen::CreateRustGen(RustGenSettings & settings){
        return new RustGen(settings);
    }
}
