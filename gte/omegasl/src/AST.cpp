#include "AST.h"

#include "Toks.def"

namespace omegasl::ast {

    namespace builtins {

        bool builtinsOn = false;

        Scope *global_scope;
        Type *void_type;
        Type *int_type;

        Type *float_type;
        Type *float2_type;
        Type *float3_type;
        Type *float4_type;

        Type *uint_type;
        Type *uint2_type;
        Type *uint3_type;

        Type *buffer_type;
        Type *texture1d_type;
        Type *texture2d_type;
        Type *texture3d_type;

        Type *sampler2d_type;
        Type *sampler3d_type;

        FuncType *make_float2;
        FuncType *make_float3;
        FuncType *make_float4;

        FuncType *dot;
        FuncType *cross;

        FuncType *write;
        FuncType *read;

        FuncType *sample;


        void Initialize(){
            if(!builtinsOn){
                builtinsOn = true;
                global_scope = Scope::Create("global",nullptr);
                void_type = new Type{KW_TY_VOID,global_scope};
                int_type = new Type{KW_TY_INT,global_scope};

                float_type = new Type{KW_TY_FLOAT,global_scope};
                float2_type = new Type{KW_TY_FLOAT2,global_scope};
                float3_type = new Type{KW_TY_FLOAT3,global_scope};
                float4_type = new Type{KW_TY_FLOAT4,global_scope};

                uint_type = new Type{KW_TY_UINT,global_scope};
                uint2_type = new Type{KW_TY_UINT2,global_scope};
                uint3_type = new Type{KW_TY_UINT3,global_scope};

                buffer_type = new Type{KW_TY_BUFFER,global_scope,true,{"type"}};
                texture1d_type = new Type{KW_TY_TEXTURE1D,global_scope};
                texture2d_type = new Type{KW_TY_TEXTURE2D,global_scope};
                texture3d_type = new Type{KW_TY_TEXTURE3D,global_scope};

                sampler2d_type = new Type{KW_TY_SAMPLER2D,global_scope};
                sampler3d_type = new Type{KW_TY_SAMPLER3D,global_scope};

                make_float2 = new FuncType{BUILTIN_MAKE_FLOAT2,global_scope,true,{},{

                    },TypeExpr::Create(float2_type)};

                make_float3 = new FuncType{BUILTIN_MAKE_FLOAT3,global_scope,true,{},{

                    },TypeExpr::Create(float3_type)};

                make_float4 = new FuncType{BUILTIN_MAKE_FLOAT4,global_scope,true,{},{

                    },TypeExpr::Create(float4_type)};

                dot = new FuncType {BUILTIN_DOT,global_scope,true,{},{
                    {"a",TypeExpr::Create("MATRIX_TYPE")},
                    {"b",TypeExpr::Create("MATRIX_TYPE")}
                    },TypeExpr::Create("SCALAR_TYPE")};

                cross = new FuncType{BUILTIN_CROSS,global_scope,true,{},{
                    {"a",TypeExpr::Create("VECTOR_TYPE")},
                    {"b",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create("VECTOR_TYPE")};

                write = new FuncType {BUILTIN_WRITE,global_scope,true,{},{
                    {"dest",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")},
                    {"data",TypeExpr::Create("VECTOR_TYPE")},
                    },TypeExpr::Create(void_type)};

                sample = new FuncType {BUILTIN_SAMPLE,global_scope,true,{},{
                    {"sampler",TypeExpr::Create("SAMPLER_TYPE")},
                    {"texture",TypeExpr::Create("TEXTURE_TYPE")},
                    {"coord",TypeExpr::Create("VECTOR_TYPE")}
                    },TypeExpr::Create(float4_type)};
            }
        }

        void Cleanup(){
            if(builtinsOn){
                builtinsOn = false;
                delete global_scope;
                delete void_type;
                delete int_type;
                delete float_type;
                delete float2_type;
                delete float3_type;
                delete float4_type;
                delete uint_type;
                delete uint2_type;
                delete uint3_type;
                delete buffer_type;
                delete texture1d_type;
                delete texture2d_type;
                delete texture3d_type;
                delete sampler2d_type;
                delete sampler3d_type;
                delete make_float2;
                delete make_float3;
                delete make_float4;
                delete dot;
                delete cross;
                delete write;
                delete sample;
            }
        }

    }

    Scope *Scope::Create(OmegaCommon::StrRef name, Scope *parent) {
        return new Scope{name,parent};
    }

    bool LiteralExpr::isFloat() const {
        return f_num.has_value();
    }

    bool LiteralExpr::isDouble() const {
        return d_num.has_value();
    }

    bool LiteralExpr::isInt() const {
        return i_num.has_value();
    }

    bool LiteralExpr::isUint() const {
        return ui_num.has_value();
    }

    bool LiteralExpr::isString() const {
        return str.has_value();
    }

    bool Scope::isParentScope(ast::Scope *scope) const {
        ast::Scope *current = parent;
        bool success = false;
        while(current != nullptr){
            current = parent->parent;
            if(scope == current){
                success = true;
                break;
            }
        }
        return success;
    }

    TypeExpr *TypeExpr::Create(OmegaCommon::StrRef name, bool pointer,bool hasTypeArgs,OmegaCommon::Vector<TypeExpr *> * args) {
        if(hasTypeArgs){
            return new TypeExpr {name, pointer,hasTypeArgs,*args};
        }
        else {
            return new TypeExpr {name,pointer,hasTypeArgs};
        }

    }

    TypeExpr *TypeExpr::Create(Type *type, bool pointer) {
        return new TypeExpr{type->name, pointer};
    }

    bool TypeExpr::compare(TypeExpr *other) {
       return (pointer == other->pointer) && (name == other->name);
    }

    TypeExpr::~TypeExpr() {
        if(hasTypeArgs)
            for(auto el : args){
                delete el;
            }
    }
}