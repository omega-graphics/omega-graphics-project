#include "wrapper_gen.h"

#include <iostream>
#include <filesystem>

namespace OmegaWrapGen {
#define SELF_REFERENCE_VAR "__self"
#define DEFINE_C_CLASS_TEMPLATE "typedef struct @{0} * @{1};"

    typedef const char *CString;

    static CString voidType = "void";
    static CString intType = "int";
    static CString floatType = "float";




    class CGen final : public Gen {
        GenContext *ctxt;

        std::ofstream outSrc;
        std::ofstream outHeader;

        std::string headerGuard;

        CGenSettings & settings;
    public:

        CGen(CGenSettings & settings):settings(settings){

        };

        void start(){
            auto fileName = ctxt->name;
            auto src = fileName + ".cpp";
            outSrc.open(std::string(ctxt->output_dir).append("/").append(src));
            auto header = fileName + ".h";
            auto header_path = std::string(ctxt->output_dir).append("/").append(header);
            outHeader.open(header_path);
            outHeader << "//" << COMMENT_HEADER << std::endl;
            std::ostringstream headerGuardBuilder;
            headerGuardBuilder << std::uppercase << "_" << fileName << "_H_" << std::nouppercase << std::endl;
            headerGuard = headerGuardBuilder.str();
            outHeader << "#ifndef " << headerGuard << std::endl;
            outHeader << "#define " << headerGuard << std::endl;
//            outSrc << OmegaCommon::fmtString(HEADER_INSERT_TEMPLATE,header) << std::endl;
        }

        void setContext(GenContext &ctxt) override {
            this->ctxt = &ctxt;
            start();
        };

        GenContext & getContext() override {
            return *ctxt;
        };

        inline std::string createCNameForDeclWithNameAndScope(OmegaCommon::StrRef name, TreeScope *scope){
            std::ostringstream out;
            std::vector<OmegaCommon::StrRef> parentScopes;
            TreeScope *parent;
            if(scope != GLOBAL_SCOPE) {
                parentScopes.emplace_back(scope->name);
                while((parent = scope->parentScope) != GLOBAL_SCOPE){
                    if(parent->type == TreeScope::Namespace || parent->type == TreeScope::Class){
                        parentScopes.emplace_back(parent->name);
                    }
                    scope = parent;
                }

                for(auto r_it = parentScopes.rbegin();r_it != parentScopes.rend();r_it++){
                    out << r_it->data();
                }
            }
            out << "__" << name.data();
            return out.str();
        }

        std::string createCNameForDecl(ClassDeclNode *node){
            return createCNameForDeclWithNameAndScope(node->name,node->scope);
        };

        std::string createCNameForDecl(FuncDeclNode *node){
            return createCNameForDeclWithNameAndScope(node->name,node->scope);
        };

        inline std::string treeTypeToString(Type *type){
            if(type == stdtypes::VOID){
                return voidType;
            }
            else if(type == stdtypes::INT){
                return intType;
            }
            else {
                std::ostringstream out;
                if(type->isConst){
                    out << "const ";
                }
                out << type->getName().data();
                if(type->isPointer || type->isReference){
                    out << "*";
                }
                return out.str();
            }
        };

        void writeCFunctionDecl(OmegaCommon::StrRef name, OmegaCommon::MapVec<OmegaCommon::String,Type *> & params, Type *returnType, std::ostream & out){
            out << treeTypeToString(returnType) << " " << name << "(";
            auto param_it = params.begin();
            for(unsigned i = 0;i < params.size();i++){
                if(i != 0) {
                    out << ",";
                }
                auto & param_type_pair = *param_it;
                out << treeTypeToString(param_type_pair.second) << " " << param_type_pair.first;
                ++param_it;
            }
            out << ")";
        }
        /** @brief Generates C Code for DeclNode consumed.
         * */
        void consumeDecl(DeclNode *node) override {
            switch (node->type) {
                case HEADER_DECL : {
                    auto * header_decl = (HeaderDeclNode *)node;
                    auto relative_path = std::filesystem::path(header_decl->name).lexically_relative(ctxt->output_dir.data());
                    outSrc << OmegaCommon::fmtString(HEADER_INSERT_TEMPLATE,relative_path.string()) << std::endl;
                    break;
                }
                case CLASS_DECL : {
                    auto * class_decl = (ClassDeclNode *)node;
                    auto struct_name = createCNameForDecl(class_decl);
                    outHeader << OmegaCommon::fmtString(DEFINE_C_CLASS_TEMPLATE,struct_name,struct_name.substr(2,struct_name.size() - 2)) << std::endl << std::endl;
                    outSrc << "struct " << struct_name << "{" << generateCXXName(class_decl->name,class_decl->scope) << " obj;};" << std::endl << std::endl;
                    for(auto & method : class_decl->instMethods){
                        auto func_name = createCNameForDecl(method);
                        method->params.insert(std::make_pair(SELF_REFERENCE_VAR,Type::Create(struct_name,false,true)));
                        writeCFunctionDecl(func_name,method->params,method->returnType,outHeader);
                        outHeader << ";" << std::endl << std::endl;
                        outSrc << "extern \"C\" ";
                        writeCFunctionDecl(func_name,method->params,method->returnType,outSrc);
                        outSrc << "{" << std::endl;
                        /// Write C Name and then CXX Func Name!
                        outSrc << "return " SELF_REFERENCE_VAR "->obj." << method->name << "(";
                        unsigned c = 0;
                        for(auto & param_type_pair : method->params){
                            if(param_type_pair.first != SELF_REFERENCE_VAR){
                                if(c != 0){
                                    outSrc << ",";
                                };
                                outSrc << param_type_pair.first;
                                c++;
                            }
                        }
                        outSrc << ");" << std::endl;
                        outSrc << "}" << std::endl << std::endl;
                    }
                    break;
                }
            }
        };


        void finish() override {
            outSrc.close();
            outHeader << "#endif //" << headerGuard << std::endl;
            outHeader.close();
        };

    };

    Gen *Gen::CreateCGen(CGenSettings & settings){
        return new CGen(settings);
    };
};