#include "wrapper_gen.h"
#include <fstream>

namespace OmegaWrapGen {
    class JavaGen final : public Gen {
        JavaGenSettings & settings;
        GenContext *context;
        std::ofstream out;
        unsigned decl_count = 0;
    public:
        explicit JavaGen(JavaGenSettings & settings):settings(settings),context(nullptr){

        }

        GenContext & getContext() override {
            return *context;
        }

        void setContext(GenContext &ctxt) override {
            context = &ctxt;
            auto output_file = std::string(ctxt.output_dir).append("/").append(ctxt.name).append(".java");
            out.open(output_file,std::ios::out);
            out << "// " << COMMENT_HEADER << "\n";
            out << "public final class " << ctxt.name << "Wrapper {\n";
        }

        void consumeDecl(DeclNode *node) override {
            (void)node;
            ++decl_count;
        }

        void finish() override {
            if(out.is_open()){
                out << "    public static final int DECL_COUNT = " << decl_count << ";\n";
                out << "}\n";
                out.close();
            }
        }
    };

    Gen *Gen::CreateJavaGen(JavaGenSettings & settings){
        return new JavaGen(settings);
    }
}
