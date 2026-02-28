#include "wrapper_gen.h"

#include <iostream>
#include <filesystem>
#include <unordered_map>
#include <cctype>

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
        std::unordered_map<std::string,std::string> arrayViewAliases;

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
                    if(parent->type == TreeScope::Namespace || parent->type == TreeScope::Class || parent->type == TreeScope::Interface){
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

        std::string createCNameForDecl(InterfaceDeclNode *node){
            return createCNameForDeclWithNameAndScope(node->name,node->scope);
        };

        std::string createCNameForDecl(StructDeclNode *node){
            return createCNameForDeclWithNameAndScope(node->name,node->scope);
        };

        std::string createClassMemberPrefix(ClassDeclNode *class_decl){
            std::ostringstream out;
            std::vector<OmegaCommon::StrRef> parentScopes;
            TreeScope *scope = class_decl->scope;
            TreeScope *parent;
            if(scope != GLOBAL_SCOPE){
                parentScopes.emplace_back(scope->name);
                while((parent = scope->parentScope) != GLOBAL_SCOPE){
                    if(parent->type == TreeScope::Namespace || parent->type == TreeScope::Class || parent->type == TreeScope::Interface){
                        parentScopes.emplace_back(parent->name);
                    }
                    scope = parent;
                }
                for(auto r_it = parentScopes.rbegin();r_it != parentScopes.rend();r_it++){
                    out << r_it->data();
                }
            }
            out << class_decl->name.data();
            return out.str();
        }

        std::string createCNameForClassFieldAccessor(ClassDeclNode *class_decl,OmegaCommon::StrRef field_name,bool is_setter){
            std::ostringstream out;
            out << createClassMemberPrefix(class_decl) << "__" << (is_setter ? "set_" : "get_") << field_name.data();
            return out.str();
        }

        std::string normalizedTypeForAlias(const std::string & c_type){
            std::ostringstream out;
            for(auto ch : c_type){
                if(std::isalnum(static_cast<unsigned char>(ch))){
                    out << ch;
                }
                else if(ch == '*'){
                    out << "Ptr";
                }
            }
            auto normalized = out.str();
            if(normalized.empty()){
                normalized = "Any";
            }
            return normalized;
        }

        std::string ensureArrayViewAlias(Type *array_type){
            auto elem_c_type = treeTypeToString(array_type->getElementType());
            auto it = arrayViewAliases.find(elem_c_type);
            if(it != arrayViewAliases.end()){
                return it->second;
            }

            auto alias = std::string("OmegaArray_").append(normalizedTypeForAlias(elem_c_type));
            unsigned suffix = 2;
            for(;;){
                bool collision = false;
                for(const auto & pair : arrayViewAliases){
                    if(pair.second == alias && pair.first != elem_c_type){
                        collision = true;
                        break;
                    }
                }
                if(!collision){
                    break;
                }
                alias = std::string("OmegaArray_").append(normalizedTypeForAlias(elem_c_type)).append("_").append(std::to_string(suffix++));
            }

            arrayViewAliases.emplace(elem_c_type,alias);
            outHeader << "typedef struct " << alias << "{" << elem_c_type << " *data; long len;} " << alias << ";" << std::endl;
            return alias;
        }

        inline std::string treeTypeToString(Type *type){
            if(type->isArray){
                return ensureArrayViewAlias(type);
            }

            auto ty_name = std::string(type->getName().data());

            if(ty_name == stdtypes::VOID->getName().data()){
                return voidType;
            }
            else if(ty_name == stdtypes::INT->getName().data()){
                return intType;
            }
            else if(ty_name == stdtypes::FLOAT->getName().data()){
                return floatType;
            }
            else if(ty_name == stdtypes::STRING->getName().data()){
                return "const char *";
            }
            else {
                std::ostringstream out;
                if(type->isConst){
                    out << "const ";
                }
                out << ty_name;
                if(type->isPointer || type->isReference){
                    out << "*";
                }
                return out.str();
            }
        };

        bool isVoidType(Type *type){
            return type->getName() == "void" && !type->isPointer && !type->isReference;
        }

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

        void writeCFunctionDeclWithInterfaceHandle(OmegaCommon::StrRef name,
                                                   OmegaCommon::StrRef iface_type,
                                                   OmegaCommon::MapVec<OmegaCommon::String,Type *> & params,
                                                   Type *returnType,
                                                   std::ostream & out){
            out << treeTypeToString(returnType) << " " << name << "(" << iface_type.data() << " iface";
            for(auto & param_type_pair : params){
                out << "," << treeTypeToString(param_type_pair.second) << " " << param_type_pair.first;
            }
            out << ")";
        }

        void writeFunctionCallArgs(OmegaCommon::MapVec<OmegaCommon::String,Type *> & params,std::ostream &out,const char *skipParam = nullptr){
            unsigned c = 0;
            for(auto & param_type_pair : params){
                if(skipParam && param_type_pair.first == skipParam){
                    continue;
                }
                if(c != 0){
                    out << ",";
                }
                out << param_type_pair.first;
                c++;
            }
        }

        void consumeCXXDataStructMethod(OmegaWrapGen::FuncDeclNode * method,std::string & struct_name){
            auto func_name = createCNameForDecl(method);
            method->params.insert(std::make_pair(SELF_REFERENCE_VAR,Type::Create(struct_name,false,true)));
            writeCFunctionDecl(func_name,method->params,method->returnType,outHeader);
            outHeader << ";" << std::endl << std::endl;
            outSrc << "extern \"C\" ";
            writeCFunctionDecl(func_name,method->params,method->returnType,outSrc);
            outSrc << "{" << std::endl;
            /// Write C Name and then CXX Func Name!
            outSrc << "return " SELF_REFERENCE_VAR "->obj." << method->name << "(";
            writeFunctionCallArgs(method->params,outSrc,SELF_REFERENCE_VAR);
            outSrc << ");" << std::endl;
            outSrc << "}" << std::endl << std::endl;
        }

        void consumeCXXDataStructField(ClassDeclNode *class_decl,const ClassField &field,std::string & struct_name){
            auto field_name = std::string(field.name.data());
            auto getter_name = createCNameForClassFieldAccessor(class_decl,field.name,false);
            auto field_ty = treeTypeToString(field.type);

            outHeader << field_ty << " " << getter_name << "(" << struct_name << "* " SELF_REFERENCE_VAR << ");" << std::endl << std::endl;

            outSrc << "extern \"C\" " << field_ty << " " << getter_name << "(" << struct_name << "* " SELF_REFERENCE_VAR << "){" << std::endl;
            outSrc << "return " SELF_REFERENCE_VAR "->obj." << field_name << ";" << std::endl;
            outSrc << "}" << std::endl << std::endl;

            if(field.type->isConst){
                return;
            }

            auto setter_name = createCNameForClassFieldAccessor(class_decl,field.name,true);
            outHeader << "void " << setter_name << "(" << struct_name << "* " SELF_REFERENCE_VAR << "," << field_ty << " value);" << std::endl << std::endl;

            outSrc << "extern \"C\" void " << setter_name << "(" << struct_name << "* " SELF_REFERENCE_VAR << "," << field_ty << " value){" << std::endl;
            outSrc << SELF_REFERENCE_VAR "->obj." << field_name << " = value;" << std::endl;
            outSrc << "}" << std::endl << std::endl;
        }

        void consumeCXXFreeFunction(OmegaWrapGen::FuncDeclNode *func_decl){
            auto func_name = createCNameForDecl(func_decl);
            writeCFunctionDecl(func_name,func_decl->params,func_decl->returnType,outHeader);
            outHeader << ";" << std::endl << std::endl;

            outSrc << "extern \"C\" ";
            writeCFunctionDecl(func_name,func_decl->params,func_decl->returnType,outSrc);
            outSrc << "{" << std::endl;
            outSrc << "return " << generateCXXName(func_decl->name,func_decl->scope) << "(";
            writeFunctionCallArgs(func_decl->params,outSrc);
            outSrc << ");" << std::endl;
            outSrc << "}" << std::endl << std::endl;
        }

        void consumeNamespaceDecl(NamespaceDeclNode *namespace_decl){
            for(auto decl : namespace_decl->body){
                consumeDecl(decl);
            }
        }

        void consumeStructDecl(StructDeclNode *struct_decl){
            // Ensure any array aliases used by fields are emitted before the struct body.
            for(auto &field : struct_decl->fields){
                (void)treeTypeToString(field.type);
            }

            auto struct_name = createCNameForDecl(struct_decl);
            auto public_struct_name = (struct_name.rfind("__",0) == 0) ? struct_name.substr(2,struct_name.size() - 2) : struct_name;
            outHeader << "typedef struct " << struct_name << " " << public_struct_name << ";" << std::endl;
            outHeader << "struct " << struct_name << "{" << std::endl;
            for(auto &field : struct_decl->fields){
                outHeader << "    " << treeTypeToString(field.type) << " " << field.name << ";" << std::endl;
            }
            outHeader << "};" << std::endl << std::endl;
        }

        void consumeInterfaceDecl(InterfaceDeclNode *interface_decl){
            // Ensure any array aliases used by methods are emitted before interface/vtable structs.
            for(auto method : interface_decl->instMethods){
                (void)treeTypeToString(method->returnType);
                for(auto &param_type_pair : method->params){
                    (void)treeTypeToString(param_type_pair.second);
                }
            }

            auto iface_struct_name = createCNameForDecl(interface_decl);
            auto public_iface_name = (iface_struct_name.rfind("__",0) == 0) ? iface_struct_name.substr(2,iface_struct_name.size() - 2) : iface_struct_name;
            auto vtable_struct_name = iface_struct_name + "VTable";

            outHeader << "typedef struct " << iface_struct_name << " " << public_iface_name << ";" << std::endl;
            outHeader << "typedef struct " << vtable_struct_name << " " << vtable_struct_name << ";" << std::endl;
            outHeader << "struct " << iface_struct_name << "{void *self; const " << vtable_struct_name << " *vtable;};" << std::endl;
            outHeader << "struct " << vtable_struct_name << "{" << std::endl;
            for(auto method : interface_decl->instMethods){
                outHeader << "    " << treeTypeToString(method->returnType) << " (*" << method->name << ")(void *self";
                for(auto &param_type_pair : method->params){
                    outHeader << "," << treeTypeToString(param_type_pair.second) << " " << param_type_pair.first;
                }
                outHeader << ");" << std::endl;
            }
            outHeader << "};" << std::endl << std::endl;

            for(auto method : interface_decl->instMethods){
                auto wrapper_name = createCNameForDecl(method);
                writeCFunctionDeclWithInterfaceHandle(wrapper_name,public_iface_name,method->params,method->returnType,outHeader);
                outHeader << ";" << std::endl << std::endl;

                outSrc << "extern \"C\" ";
                writeCFunctionDeclWithInterfaceHandle(wrapper_name,public_iface_name,method->params,method->returnType,outSrc);
                outSrc << "{" << std::endl;
                if(isVoidType(method->returnType)){
                    outSrc << "iface.vtable->" << method->name << "(iface.self";
                }
                else {
                    outSrc << "return iface.vtable->" << method->name << "(iface.self";
                }
                for(auto &param_type_pair : method->params){
                    outSrc << "," << param_type_pair.first;
                }
                outSrc << ");" << std::endl;
                outSrc << "}" << std::endl << std::endl;
            }
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
                case NAMESPACE_DECL : {
                    auto *namespace_decl = (NamespaceDeclNode *)node;
                    consumeNamespaceDecl(namespace_decl);
                    break;
                }
                case CLASS_DECL : {
                    auto * class_decl = (ClassDeclNode *)node;
                    auto struct_name = createCNameForDecl(class_decl);
                    auto public_struct_name = (struct_name.rfind("__",0) == 0) ? struct_name.substr(2,struct_name.size() - 2) : struct_name;
                    outHeader << OmegaCommon::fmtString(DEFINE_C_CLASS_TEMPLATE,struct_name,public_struct_name) << std::endl << std::endl;
                    outSrc << "struct " << struct_name << "{" << generateCXXName(class_decl->name,class_decl->scope) << " obj;};" << std::endl << std::endl;
                    for(auto & field : class_decl->fields){
                        consumeCXXDataStructField(class_decl,field,struct_name);
                    }
                    for(auto & method : class_decl->instMethods){
                        consumeCXXDataStructMethod(method,struct_name);
                    }
                    break;
                }
                case FUNC_DECL : {
                    auto *func_decl = (FuncDeclNode *)node;
                    consumeCXXFreeFunction(func_decl);
                    break;
                }
                case INTERFACE_DECL : {
                    auto *interface_decl = (InterfaceDeclNode *)node;
                    consumeInterfaceDecl(interface_decl);
                    break;
                }
                case STRUCT_DECL : {
                    auto *struct_decl = (StructDeclNode *)node;
                    consumeStructDecl(struct_decl);
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
