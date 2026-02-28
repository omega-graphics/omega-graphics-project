#include "wrapper_gen.h"
#include <fstream>

namespace OmegaWrapGen {
    class GoGen final : public Gen {
        GoGenSettings & settings;
        GenContext *context;
        std::ofstream out;
        unsigned decl_count = 0;
    public:
        explicit GoGen(GoGenSettings & settings):settings(settings),context(nullptr){

        }

        GenContext & getContext() override {
            return *context;
        }

        void setContext(GenContext &ctxt) override {
            context = &ctxt;
            auto output_file = std::string(ctxt.output_dir).append("/").append(ctxt.name).append(".go");
            out.open(output_file,std::ios::out);
            out << "// " << COMMENT_HEADER << "\n";
            out << "package wrappers\n\n";
        }

        void consumeDecl(DeclNode *node) override {
            (void)node;
            ++decl_count;
        }

        void finish() override {
            if(out.is_open()){
                out << "const DeclCount = " << decl_count << "\n";
                out.close();
            }
        }
    };

    Gen *Gen::CreateGoGen(GoGenSettings & settings){
        return new GoGen(settings);
    }
}
