#include "wrapper_gen.h"

namespace OmegaWrapGen {
    class PythonGen final : public Gen {
        PythonGenSettings & settings;
        GenContext *context;
    public:
        explicit PythonGen(PythonGenSettings & settings):settings(settings),context(nullptr){

        }
        GenContext & getContext() override {
            return *context;
        }
        void setContext(GenContext &ctxt) override {
            context = &ctxt;
        }
        void consumeDecl(DeclNode *node) override {

        }
        void finish() override {

        }
    };

    Gen *Gen::CreatePythonGen(PythonGenSettings &settings) {
        return new PythonGen(settings);
    }
}