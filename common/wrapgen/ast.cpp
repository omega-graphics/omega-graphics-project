#include "ast.h"
#include <sstream>

namespace OmegaWrapGen {

    TreeScope *GLOBAL_SCOPE = new TreeScope {TreeScope::Namespace,"__GLOBAL__",nullptr};

      namespace stdtypes {

        Type * VOID = Type::Create("void");
        Type * INT = Type::Create("int");
        Type * FLOAT = Type::Create("float");
        Type * LONG = Type::Create("long");
        Type * DOUBLE = Type::Create("double");
        Type * CHAR = Type::Create("char");
        /// ADT

        Type *STRING = Type::Create("string");
        Type *VECTOR = Type::Create("vector");
        Type *MAP = Type::Create("map");
    };


    

    Type * Type::Create(OmegaCommon::StrRef name, bool isConst, bool isPointer, bool isReference){
        auto t = new Type();
        t->name = name.data();
        t->isConst = isConst;
        t->isPointer = isPointer;
        t->isReference = isReference;
        return t;
    };

    OmegaCommon::StrRef Type::getName(){
        return name;
    };

    TreeDumper::TreeDumper(std::ostream & out):out(out){

    };
   
    inline OmegaCommon::String padString(unsigned lvl){
        std::ostringstream out;
        for(;lvl > 0;lvl--)
            out << "  " << std::flush;
        return out.str();
    };  

    inline OmegaCommon::String ASTTypeToString(Type *type){
        std::ostringstream out;
        out << type->getName().data() << std::flush;
        return out.str();
    };

    void TreeDumper::writeDecl(DeclNode *node,unsigned level){
        auto pad = padString(level);
        switch (node->type) {
            case NAMESPACE_DECL : {
                auto * decl_node = (NamespaceDeclNode *)node;
                out << pad << 
                "NamespaceDecl : {\n" << pad <<
                "   name:" << decl_node->name << "\n" << pad <<
                "   body:[\n" << std::flush;
                for(auto n : decl_node->body){
                    if(n != decl_node->body.front()) out << ",\n";
                    writeDecl(n,level + 2);
                    out << std::flush;
                };
                out << 
                pad << "   ]\n" << pad <<
                "}" << std::endl;
                break;
            }
            case FUNC_DECL : {
                auto * func_node = (FuncDeclNode *)node;
                out << pad << 
                "FuncDecl : {\n" << pad <<
                "   name:" << func_node->name << "\n" << pad <<
                "   params:{\n" << std::flush;
                auto second_pad = padString(level + 1);
                for(const auto& n : func_node->params){
                    if(n.first != func_node->params.begin()->first) out << ",\n";
                    out << second_pad << "\"" << n.first << "\":" << ASTTypeToString(n.second) << std::flush;
                };
                out << 
                pad << "   }\n" << pad <<
                "   returnType:" << ASTTypeToString(func_node->returnType) << "\n" << pad <<
                "}" << std::endl;
                break;
            }
            case CLASS_DECL : {
                auto * decl_node = (ClassDeclNode *)node;
                out << pad << 
                "ClassDecl : {\n" << pad <<
                "   name:" << decl_node->name << "\n" << pad <<
                /// Write Instance Methods
                "   instMethods:[\n" << std::flush;
                for(auto n : decl_node->instMethods){
                    if(n != decl_node->instMethods.front()) out << ",\n";
                    writeDecl(n,level + 2);
                    out << std::flush;
                };
                out << 
                pad << "   ]\n" << pad <<
                /// Write Static Methods
                "   staticMethods:[\n" << std::flush;
                for(auto n : decl_node->staticMethods){
                    if(n != decl_node->staticMethods.front()) out << ",\n";
                    writeDecl(n,level + 2);
                    out << std::flush;
                };
                out << 
                pad << "   ]\n" << pad <<
                "}" << std::endl;
                break;
            }
            case INTERFACE_DECL : {
                auto * decl_node = (InterfaceDeclNode *)node;
                out << pad << 
                "InterfaceDecl : {\n" << pad <<
                "   name:" << decl_node->name << "\n" << pad <<
                /// Write Instance Methods
                "   instMethods:[\n" << std::flush;
                for(auto n : decl_node->instMethods){
                    if(n != decl_node->instMethods.front()) out << ",\n";
                    writeDecl(n,level + 2);
                    out << std::flush;
                };
                out << 
                pad << "   ]\n" << pad <<
                "}" << std::endl;
                break;
            }
            case HEADER_DECL : {
                auto * decl_node = (HeaderDeclNode *)node;
                out << pad << "HeaderDecl : {" << std::endl << pad << "   header:\"" << decl_node->name << "\"" << std::endl << pad << "}" << std::endl;
                break;
            }
        }
    };

    void TreeDumper::consumeDecl(DeclNode *node){
        writeDecl(node,0);
    };
};