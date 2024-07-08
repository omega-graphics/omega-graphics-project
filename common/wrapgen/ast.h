#include "nodes.def"
#include <vector>
#include <string>

#include "omega-common/common.h"

#ifndef OMEGA_WRAPGEN_AST_H
#define OMEGA_WRAPGEN_AST_H

namespace OmegaWrapGen {

    struct TreeScope {
        typedef enum : int {
            Class,
            Func,
            Interface,
            Namespace
        } Ty;
        Ty type;
        OmegaCommon::String name;
        TreeScope *parentScope = nullptr;
    };

    extern TreeScope *GLOBAL_SCOPE;

    struct DeclNode {
        TreeNodeType type;
        TreeScope * scope;
    };

    class Type {
        OmegaCommon::String name;
    public:
        bool isConst;

        bool isPointer;

        bool isReference;

        static Type * Create(OmegaCommon::StrRef name, bool isConst = false, bool isPointer = false, bool isReference = false);
        OmegaCommon::StrRef getName();
    };

    #define _STANDARD_TYPE(name) extern Type *name

    namespace stdtypes {

        _STANDARD_TYPE(VOID);
        _STANDARD_TYPE(INT);
        _STANDARD_TYPE(FLOAT);
        _STANDARD_TYPE(LONG);
        _STANDARD_TYPE(DOUBLE);
        _STANDARD_TYPE(CHAR);
        /// ADT

        _STANDARD_TYPE(STRING);
        _STANDARD_TYPE(VECTOR);
        _STANDARD_TYPE(MAP);
    };

    struct HeaderDeclNode : public DeclNode {
        OmegaCommon::String name;
    };

    struct NamespaceDeclNode : public DeclNode {
        OmegaCommon::String name;
        OmegaCommon::Vector<DeclNode *> body;
    };

    struct FuncDeclNode : public DeclNode {
        OmegaCommon::String name;
        bool isStatic = false;
        bool isConstructor = false;
        OmegaCommon::MapVec<OmegaCommon::String,Type *> params;
        Type *returnType;
    };

    struct ClassDeclNode : public DeclNode {
        OmegaCommon::String name;
        OmegaCommon::Vector<FuncDeclNode *> instMethods;
        OmegaCommon::Vector<FuncDeclNode *> staticMethods;
    };

    struct InterfaceDeclNode : public DeclNode {
        OmegaCommon::String name;
        OmegaCommon::Vector<FuncDeclNode *> instMethods;
    };

    class TreeConsumer {
    public:
        virtual void consumeDecl(DeclNode *node) = 0;
    };

    class TreeDumper : public TreeConsumer {
        std::ostream & out;
    public:
        void writeDecl(DeclNode *node,unsigned level);
        void consumeDecl(DeclNode *node) override;
        TreeDumper(std::ostream & out);
    };

};

#endif