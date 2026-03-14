#include "wrapper_gen.h"
#include <fstream>

namespace OmegaWrapGen {

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


    Gen *Gen::CreateRustGen(RustGenSettings & settings){
        return new RustGen(settings);
    }

};