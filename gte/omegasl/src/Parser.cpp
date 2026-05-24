#include "Parser.h"
#include "Sema.h"
#include "ConstFold.h"
#include "CodeGen.h"

#include <limits>

namespace omegasl {

    /// Operator precedence for binary expressions (higher = tighter binding).
    /// C-style ladder: assign < logical-or < logical-and < bitwise-or < bitwise-xor
    /// < bitwise-and < equality < relational < shift < additive < multiplicative.
    static int getBinaryPrecedence(const Tok &t) {
        if (t.type == TOK_ASTERISK) return 10;
        /// TOK_AMPERSAND carries the single `&` character — address-of in
        /// prefix position, bitwise-AND in binary position. The parser's
        /// prefix branch consumes it before this runs, so reaching here
        /// means it's being used as a binary operator.
        if (t.type == TOK_AMPERSAND) return 5;
        if (t.type != TOK_OP) return -1;
        if (t.str == OP_DIV || t.str == "*" || t.str == OP_MOD) return 10;
        if (t.str == OP_PLUS || t.str == OP_MINUS) return 9;
        if (t.str == OP_LSHIFT || t.str == OP_RSHIFT) return 8;
        if (t.str == OP_LESS || t.str == OP_LESSEQUAL ||
            t.str == OP_GREATER || t.str == OP_GREATEREQUAL) return 7;
        if (t.str == OP_ISEQUAL || t.str == OP_NOTEQUAL) return 6;
        if (t.str == OP_BITAND) return 5;
        if (t.str == OP_BITXOR) return 4;
        if (t.str == OP_BITOR) return 3;
        if (t.str == OP_LOGAND) return 2;
        if (t.str == OP_LOGOR) return 1;
        if (t.str == OP_EQUAL || t.str == OP_PLUSEQUAL || t.str == OP_MINUSEQUAL ||
            t.str == OP_MULEQUAL || t.str == OP_DIVEQUAL || t.str == OP_MODEQUAL ||
            t.str == OP_ANDEQUAL || t.str == OP_OREQUAL || t.str == OP_XOREQUAL ||
            t.str == OP_LSHIFTEQUAL || t.str == OP_RSHIFTEQUAL) return 0;
        return -1;
    }
    static OmegaCommon::String getBinaryOpStr(const Tok &t) {
        if (t.type == TOK_ASTERISK) return "*";
        if (t.type == TOK_AMPERSAND) return "&";
        return t.str;
    }

    Parser::Parser(std::shared_ptr<CodeGen> &gen):
    lexer(std::make_unique<Lexer>()),
    gen(gen),
    sem(std::make_unique<Sem>()),tokIdx(0){
        gen->setTypeResolver(sem.get());
    }

    ast::TypeExpr *Parser::buildTypeRef(Tok &first_tok,bool isPointer,bool hasTypeArgs,OmegaCommon::Vector<ast::TypeExpr *> * args) {

        if(first_tok.type == TOK_ID || first_tok.type  == TOK_KW_TYPE){
            return ast::TypeExpr::Create(first_tok.str,isPointer,hasTypeArgs,args);
        }
        else {
            // ERROR
            return nullptr;
        }
    }

    bool Parser::parseResourceArgList(Tok & t, OmegaCommon::Vector<ResourceArgEntry> & out) {
        while(true){
            if(t.type != TOK_ID){
                auto e = std::make_unique<UnexpectedToken>(std::string("Expected identifier for resource argument name, got `") + t.str + "`");
                e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                diagnostics->addError(std::move(e));
                return false;
            }
            ResourceArgEntry entry;
            entry.nameTok = t;
            entry.name = t.str;

            t = lexer->nextTok();
            if(t.type != TOK_OP || t.str != OP_EQUAL){
                auto e = std::make_unique<UnexpectedToken>(std::string("Expected `=` after resource argument name, got `") + t.str + "`");
                e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                diagnostics->addError(std::move(e));
                return false;
            }

            t = lexer->nextTok();
            entry.valueTok = t;
            if(t.type != TOK_ID && t.type != TOK_NUM_LITERAL){
                auto e = std::make_unique<UnexpectedToken>(std::string("Expected identifier or numeric literal, got `") + t.str + "`");
                e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                diagnostics->addError(std::move(e));
                return false;
            }
            // Concatenate adjacent ID / NUM tokens that share the same line
            // and contiguous column range. The lexer splits `rg01` into
            // `rg` + `01`; merging them here lets `swizzle=rg01` parse as a
            // single value while leaving conventional `key=word` /
            // `key=N` cases unchanged. The merge is gated on column
            // adjacency so accidental whitespace (`filter=linear point`)
            // still surfaces the second word as an unexpected token.
            OmegaCommon::String concatenated = t.str;
            bool allNumeric = (t.type == TOK_NUM_LITERAL);
            unsigned prevColEnd = t.colEnd;
            unsigned prevLine = t.line;
            entry.valueTok.colEnd = t.colEnd;
            t = lexer->nextTok();
            while((t.type == TOK_ID || t.type == TOK_NUM_LITERAL)
                  && t.line == prevLine && t.colStart == prevColEnd){
                concatenated += t.str;
                if(t.type != TOK_NUM_LITERAL) allNumeric = false;
                prevColEnd = t.colEnd;
                entry.valueTok.colEnd = t.colEnd;
                t = lexer->nextTok();
            }
            entry.valueStr = concatenated;
            if(allNumeric){
                entry.valueInt = static_cast<unsigned>(std::stoul(concatenated));
                entry.valueIsNumeric = true;
            }
            out.push_back(std::move(entry));
            if(t.type == TOK_COMMA){
                t = lexer->nextTok();
                continue;
            }
            if(t.type == TOK_RPAREN){
                t = lexer->nextTok();
                return true;
            }
            auto e = std::make_unique<UnexpectedToken>(std::string("Expected `,` or `)` in resource argument list, got `") + t.str + "`");
            e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
            diagnostics->addError(std::move(e));
            return false;
        }
    }

    ast::Decl *Parser::parseGlobalDecl() {
        ast::Decl *node = nullptr;
        bool shaderDecl;
        bool staticResourceDecl = false;

        auto t = lexer->nextTok();
        if(t.type == TOK_EOF){
            return nullptr;
        }

        /// Parse Resource Map For ShaderDecl
        if(t.type == TOK_LBRACKET){
            auto _n = new ast::ShaderDecl();
            _n->type = SHADER_DECL;

            while((t = lexer->nextTok()).type != TOK_RBRACKET){

                if(t.type == TOK_COMMA){
                    t = lexer->nextTok();
                }

                /// in/out/inout are contextual keywords — lexed as TOK_ID.
                ast::ShaderDecl::ResourceMapDesc mapDesc;

                if(t.str == KW_IN){
                    mapDesc.access = ast::ShaderDecl::ResourceMapDesc::In;
                }
                else if(t.str == KW_INOUT){
                    mapDesc.access = ast::ShaderDecl::ResourceMapDesc::Inout;
                }
                else if(t.str == KW_OUT){
                    mapDesc.access = ast::ShaderDecl::ResourceMapDesc::Out;
                }
                else {
                    auto e = std::make_unique<UnexpectedToken>(std::string("Expected `in`, `out`, or `inout` in resource map, got `") + t.str + "`");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                t = lexer->nextTok();
                if(t.type != TOK_ID){
                    /// Error Expected Keyword
                    auto e = std::make_unique<UnexpectedToken>("Expected identifier for resource name");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                mapDesc.name = t.str;
                _n->resourceMap.push_back(mapDesc);

            }

            node = (ast::Decl *)_n;
            t = lexer->nextTok();
        }

        if(t.type == TOK_KW){
            /// Parse Struct
            if(t.str == KW_STRUCT && node == nullptr){
                auto _decl = new ast::StructDecl();
                _decl->type = STRUCT_DECL;
                _decl->internal = false;
                t = lexer->nextTok();
                if(t.type != TOK_ID){
                    // Expected ID.
                }
                _decl->name = t.str;

                t = lexer->nextTok();

                if(t.type == TOK_KW){
                    if(t.str != KW_INTERNAL){
                        // Expected No Keyword or Internal
                        auto e = std::make_unique<UnexpectedToken>("Expected `internal` keyword or no keyword");
                        e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                    _decl->internal = true;
                    t = lexer->nextTok();
                }

                if(t.type == TOK_LBRACE){
                    t = lexer->nextTok();
                    while(t.type != TOK_RBRACE){
                        auto _tok = t;
                        t = lexer->nextTok();
                        bool type_is_pointer = false;
                        if(t.type == TOK_ASTERISK){
                            type_is_pointer = true;
                            t = lexer->nextTok();
                        }

                        auto var_ty = buildTypeRef(_tok,type_is_pointer);
                        if(!var_ty){
                            return nullptr;
                        }

                        if(t.type != TOK_ID){
                            /// ERROR!
                            auto e = std::make_unique<UnexpectedToken>(std::string("Expected identifier, got `") + t.str + "`");
                            e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                        auto var_id = t.str;
                        t = lexer->nextTok();
                        if(t.type == TOK_COLON){
                            t = lexer->nextTok();
                            if(t.type != TOK_ID){
                                /// ERROR!
                                auto e = std::make_unique<UnexpectedToken>(std::string("Expected identifier for attribute, got `") + t.str + "`");
                                e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                                diagnostics->addError(std::move(e));
                                return nullptr;
                            }
                            auto attr_name = t.str;
                            std::optional<unsigned> attr_index;
                            t = lexer->nextTok();
                            if(t.type == TOK_LPAREN){
                                t = lexer->nextTok();
                                if(t.type != TOK_NUM_LITERAL){
                                    auto e = std::make_unique<UnexpectedToken>("Expected numeric literal for attribute index");
                                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                                    diagnostics->addError(std::move(e));
                                    return nullptr;
                                }
                                attr_index = (unsigned)std::stoul(t.str);
                                t = lexer->nextTok();
                                if(t.type != TOK_RPAREN){
                                    auto e = std::make_unique<UnexpectedToken>("Expected `)` after attribute index");
                                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                                    diagnostics->addError(std::move(e));
                                    return nullptr;
                                }
                                t = lexer->nextTok();
                            }
                            _decl->fields.push_back({var_ty,var_id,attr_name,attr_index});
                        }
                        else {
                            _decl->fields.push_back({var_ty,var_id,{},{}});
                        }

                        if(t.type != TOK_SEMICOLON){
                            /// Error. Expected Semicolon.
                            auto e = std::make_unique<UnexpectedToken>("Expected semicolon after struct field");
                            e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                        t = lexer->nextTok();
                    }

                    t = lexer->nextTok();

                    if(t.type != TOK_SEMICOLON){
                        /// Error. Expected Semicolon.
                        auto e = std::make_unique<UnexpectedToken>("Expected semicolon after struct declaration");
                        e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                    else {
                        _decl->scope = ast::builtins::global_scope;
                        return _decl;
                    }
                }
                else {
                    /// Error Unexpected Token
                    auto e = std::make_unique<UnexpectedToken>("Expected `{` after struct name");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
            }
            else if(t.str == KW_STRUCT) {
                // Error . Struct cannot have a resource map.
                auto e = std::make_unique<UnexpectedToken>("Struct cannot have a resource map");
                e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                diagnostics->addError(std::move(e));
                return nullptr;
            }

            if(node == nullptr && t.str != KW_STATIC){
                node = (ast::Decl *)new ast::ShaderDecl();
                node->type = SHADER_DECL;
            }

            if(t.str == KW_VERTEX){
                auto _s = (ast::ShaderDecl *)node;
                _s->shaderType = ast::ShaderDecl::Vertex;
            }
            else if(t.str == KW_FRAGMENT){
                auto _s = (ast::ShaderDecl *)node;
                _s->shaderType = ast::ShaderDecl::Fragment;
            }
            else if(t.str == KW_HULL || t.str == KW_DOMAIN){
                auto _s = (ast::ShaderDecl *)node;
                _s->shaderType = (t.str == KW_HULL) ? ast::ShaderDecl::Hull : ast::ShaderDecl::Domain;

                /// Parse tessellation descriptor: hull(domain=tri, partitioning=integer, outputtopology=triangle_cw, outputcontrolpoints=3)
                /// or: domain(domain=tri)
                t = lexer->nextTok();
                if(t.type == TOK_LPAREN){
                    t = lexer->nextTok();
                    while(t.type != TOK_RPAREN){
                        if(t.type != TOK_ID && t.type != TOK_KW){
                            auto e = std::make_unique<UnexpectedToken>(std::string("Expected property name in tessellation descriptor, got `") + t.str + "`");
                            e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                        auto propName = t.str;
                        t = lexer->nextTok();
                        if(t.type != TOK_OP || t.str != OP_EQUAL){
                            auto e = std::make_unique<UnexpectedToken>("Expected `=` after tessellation property name");
                            e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                        t = lexer->nextTok();
                        auto propValue = t.str;

                        if(propName == "domain"){
                            if(propValue == "tri") _s->tessDesc.domain = ast::ShaderDecl::TessellationDesc::Triangle;
                            else if(propValue == "quad") _s->tessDesc.domain = ast::ShaderDecl::TessellationDesc::Quad;
                        }
                        else if(propName == "partitioning"){
                            if(propValue == "integer") _s->tessDesc.partitioning = ast::ShaderDecl::TessellationDesc::Integer;
                            else if(propValue == "fractional_even") _s->tessDesc.partitioning = ast::ShaderDecl::TessellationDesc::FractionalEven;
                            else if(propValue == "fractional_odd") _s->tessDesc.partitioning = ast::ShaderDecl::TessellationDesc::FractionalOdd;
                        }
                        else if(propName == "outputtopology"){
                            if(propValue == "triangle_cw") _s->tessDesc.outputTopology = ast::ShaderDecl::TessellationDesc::TriangleCW;
                            else if(propValue == "triangle_ccw") _s->tessDesc.outputTopology = ast::ShaderDecl::TessellationDesc::TriangleCCW;
                            else if(propValue == "line") _s->tessDesc.outputTopology = ast::ShaderDecl::TessellationDesc::Line;
                        }
                        else if(propName == "outputcontrolpoints"){
                            _s->tessDesc.outputControlPoints = std::stoul(propValue);
                        }

                        t = lexer->nextTok();
                        if(t.type == TOK_COMMA){
                            t = lexer->nextTok();
                        }
                    }
                }
                else {
                    auto e = std::make_unique<UnexpectedToken>("Expected `(` after hull/domain keyword with tessellation descriptor");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    delete _s;
                    return nullptr;
                }
            }
            else if(t.str == KW_COMPUTE){
                auto _s = (ast::ShaderDecl *)node;
                _s->shaderType = ast::ShaderDecl::Compute;
                /// Parse ThreadGroup Desc for Compute Shader.
                /// @example [...]compute(x=1,y=1,z=1) computeShader(){...}
                t = lexer->nextTok();

                if(t.type != TOK_LPAREN){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected `(` after compute keyword");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                t = lexer->nextTok();
                if(t.type != TOK_ID && t.str != "x"){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected identifier `x`");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                t = lexer->nextTok();

                if(t.type != TOK_OP && t.str != "="){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected `=`");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                t = lexer->nextTok();
                if(t.type != TOK_NUM_LITERAL){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected numeric literal for threadgroup x dimension");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                _s->threadgroupDesc.x = std::stoi(t.str);

                t = lexer->nextTok();
                if(t.type != TOK_COMMA){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected `,` after x dimension");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }


                t = lexer->nextTok();
                if(t.type != TOK_ID && t.str != "y"){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected identifier `y`");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                t = lexer->nextTok();

                if(t.type != TOK_OP && t.str != "="){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected `=`");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                t = lexer->nextTok();
                if(t.type != TOK_NUM_LITERAL){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected numeric literal for threadgroup y dimension");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                _s->threadgroupDesc.y = std::stoi(t.str);

                t = lexer->nextTok();
                if(t.type != TOK_COMMA){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected `,` after y dimension");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }



                t = lexer->nextTok();
                if(t.type != TOK_ID && t.str != "z"){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected identifier `z`");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                t = lexer->nextTok();

                if(t.type != TOK_OP && t.str != "="){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected `=`");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                t = lexer->nextTok();
                if(t.type != TOK_NUM_LITERAL){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected numeric literal for threadgroup z dimension");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                _s->threadgroupDesc.z = std::stoi(t.str);

                t = lexer->nextTok();
                if(t.type != TOK_RPAREN){
                    delete _s;
                    auto e = std::make_unique<UnexpectedToken>("Expected `)` after threadgroup descriptor");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

            }
            else if(t.str == KW_STATIC) {
                node = (ast::Decl *)new ast::ResourceDecl();
                node->type = RESOURCE_DECL;
                ((ast::ResourceDecl *)node)->isStatic = true;
                staticResourceDecl = true;
            }
            else {
                auto e = std::make_unique<UnexpectedToken>(std::string("Unexpected keyword: ") + t.str); e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd }; diagnostics->addError(std::move(e));
                delete node;
                return nullptr;
            }
            t = lexer->nextTok();
        }

        auto _tok = t;
        t = lexer->nextTok();
        OmegaCommon::Vector<ast::TypeExpr *> type_args;
        bool type_is_pointer = false,has_type_args = false;

        if(t.type == TOK_OP && t.str == OP_LESS){
            has_type_args = true;
            t = lexer->nextTok();
            while(t.type != TOK_OP && t.str != OP_GREATER){
                /// A type arg may be a user type (`TOK_ID`) or a builtin type
                /// name (`TOK_KW_TYPE`, e.g. `buffer<float4>` / `buffer<uint>`).
                /// Dropping the builtin case left the arg list empty, which
                /// later crashed when indexing the buffer dereferenced
                /// `args[0]` out of bounds (see Sema INDEX_EXPR).
                if(t.type == TOK_ID || t.type == TOK_KW_TYPE){
                    type_args.push_back(ast::TypeExpr::Create(t.str));
                }
                t = lexer->nextTok();
            }
            t = lexer->nextTok();
        }

        if(t.type == TOK_ASTERISK){
            type_is_pointer = true;
            t = lexer->nextTok();
        }

        auto ty_for_decl = buildTypeRef(_tok,type_is_pointer,has_type_args,&type_args);

        if(t.type != TOK_ID){
            /// ERROR!
            delete node;
            auto e = std::make_unique<UnexpectedToken>(std::string("Expected identifier, got `") + t.str + "`");
            e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
            diagnostics->addError(std::move(e));
            return nullptr;
        }

        auto id_for_decl = t.str;
        t = lexer->nextTok();


        if(t.type == TOK_LPAREN){
            /// Parse ResourceDecl (For Static Samplers)
            if(staticResourceDecl){
                auto res_decl = (ast::ResourceDecl *)node;
                res_decl->typeExpr = ty_for_decl;
                res_decl->name = id_for_decl;
                ast::ResourceDecl::StaticSamplerDesc samplerDesc {};
                t = lexer->nextTok();
                OmegaCommon::Vector<ResourceArgEntry> args;
                if(!parseResourceArgList(t, args)){
                    delete res_decl;
                    return nullptr;
                }
                for(auto & arg : args){
                    if(arg.name == SAMPLER_PROP_FILTER){
                        if(arg.valueStr == SAMPLER_FILTER_LINEAR){
                            samplerDesc.filter = OMEGASL_SHADER_SAMPLER_LINEAR_FILTER;
                        }
                        else if(arg.valueStr == SAMPLER_FILTER_POINT){
                            samplerDesc.filter = OMEGASL_SHADER_SAMPLER_POINT_FILTER;
                        }
                        else if(arg.valueStr == SAMPLER_FILTER_ANISOTROPIC){
                            samplerDesc.filter = OMEGASL_SHADER_SAMPLER_MAX_ANISOTROPY_FILTER;
                        }
                    }
                    else if(arg.name == SAMPLER_PROP_ADDRESS_MODE){
                        omegasl_shader_static_sampler_address_mode addressMode = samplerDesc.uAddressMode;
                        if(arg.valueStr == SAMPLER_ADDRESS_MODE_WRAP){
                            addressMode = OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_WRAP;
                        }
                        else if(arg.valueStr == SAMPLER_ADDRESS_MODE_MIRROR){
                            addressMode = OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRROR;
                        }
                        else if(arg.valueStr == SAMPLER_ADDRESS_MODE_MIRRORWRAP){
                            addressMode = OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRRORWRAP;
                        }
                        else if(arg.valueStr == SAMPLER_ADDRESS_MODE_CLAMPTOEDGE){
                            addressMode = OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_CLAMPTOEDGE;
                        }
                        else {
                            delete res_decl;
                            auto e = std::make_unique<UnexpectedToken>(std::string("Unknown sampler address mode `") + arg.valueStr + "`");
                            e->loc = ErrorLoc{ arg.valueTok.line, arg.valueTok.line, arg.valueTok.colStart, arg.valueTok.colEnd };
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                        samplerDesc.uAddressMode = samplerDesc.vAddressMode = samplerDesc.wAddressMode = addressMode;
                    }
                    else if(arg.name == SAMPLER_PROP_MAX_ANISOTROPY){
                        samplerDesc.maxAnisotropy = arg.valueInt;
                    }
                }

                if(t.type != TOK_SEMICOLON){
                    delete res_decl;
                    auto e = std::make_unique<UnexpectedToken>(std::string("Expected semicolon, got `") + t.str + "`");
                    e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }

                auto pt = new ast::ResourceDecl::StaticSamplerDesc;
                memcpy(pt,&samplerDesc,sizeof(samplerDesc));
                res_decl->staticSamplerDesc.reset(pt);
            }
            else {
                /// Parse FuncDecl/ShaderDecl
                ast::FuncDecl *funcDecl;
                if (node == nullptr) {
                    funcDecl = new ast::FuncDecl();
                    funcDecl->type = FUNC_DECL;
                } else {
                    /// ShaderDecl is a FuncDecl!
                    funcDecl = (ast::FuncDecl *) node;
                }

                funcDecl->name = id_for_decl;
                funcDecl->returnType = ty_for_decl;
                t = lexer->nextTok();

                while (t.type != TOK_RPAREN) {
                    /// §3.7 — `in` / `out` / `inout` access qualifier. The
                    /// keywords are contextual (lexed as TOK_ID since they
                    /// are also valid identifiers), so we peek at the
                    /// string and consume the token only when it precedes
                    /// what would otherwise be a type token. `in` is the
                    /// default; the qualifier exists so HLSL `out`-style
                    /// multi-return patterns (e.g. `sincos(x, s, c)`)
                    /// have a source-level shape.
                    ast::AttributedFieldDecl::ParamAccess paramAccess =
                        ast::AttributedFieldDecl::In;
                    /// §3.6 — `const` parameter qualifier. `const` is a real
                    /// keyword (TOK_KW) while the access qualifiers are
                    /// contextual (TOK_ID), so collect both in one loop: the
                    /// two may appear in either order in front of the type
                    /// (`const in T`, `in const T`). A repeated `const` is a
                    /// duplicate error.
                    bool paramConst = false;
                    for (;;) {
                        if (t.type == TOK_KW && t.str == KW_CONST) {
                            if (paramConst) {
                                delete node;
                                auto e = std::make_unique<UnexpectedToken>("Duplicate `const` on parameter");
                                e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                                diagnostics->addError(std::move(e));
                                return nullptr;
                            }
                            paramConst = true;
                            t = lexer->nextTok();
                        } else if (t.type == TOK_ID
                                   && (t.str == KW_IN || t.str == KW_OUT || t.str == KW_INOUT)) {
                            if (t.str == KW_IN)         paramAccess = ast::AttributedFieldDecl::In;
                            else if (t.str == KW_OUT)   paramAccess = ast::AttributedFieldDecl::Out;
                            else                        paramAccess = ast::AttributedFieldDecl::Inout;
                            t = lexer->nextTok();
                        } else {
                            break;
                        }
                    }

                    auto _tok = t;
                    t = lexer->nextTok();
                    bool type_is_pointer = false;

                    if (t.type == TOK_ASTERISK) {
                        type_is_pointer = true;
                        t = lexer->nextTok();
                    }

                    auto var_ty = buildTypeRef(_tok, type_is_pointer);

                    /// §3.6 — postfix `T const name` spelling. The C-family
                    /// backends accept it; we set the same flag the prefix
                    /// form does. `const T const name` trips the duplicate
                    /// check above only for the prefix repeat — guard the
                    /// postfix repeat here too.
                    if (t.type == TOK_KW && t.str == KW_CONST) {
                        if (paramConst) {
                            delete node;
                            auto e = std::make_unique<UnexpectedToken>("Duplicate `const` on parameter");
                            e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                        paramConst = true;
                        t = lexer->nextTok();
                    }

                    if (t.type != TOK_ID) {
                        /// ERROR!
                        delete node;
                        auto e = std::make_unique<UnexpectedToken>(std::string("Expected identifier for parameter name, got `") + t.str + "`");
                        e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }

                    auto var_id = t.str;

                    t = lexer->nextTok();
                    if (t.type == TOK_COLON) {
                        t = lexer->nextTok();
                        if (t.type != TOK_ID) {
                            // Error!
                            delete node;
                            auto e = std::make_unique<UnexpectedToken>(std::string("Expected identifier for attribute, got `") + t.str + "`");
                            e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }

                        auto attr_name = t.str;
                        std::optional<unsigned> attr_index;
                        t = lexer->nextTok();
                        if(t.type == TOK_LPAREN){
                            t = lexer->nextTok();
                            if(t.type != TOK_NUM_LITERAL){
                                delete node;
                                auto e = std::make_unique<UnexpectedToken>("Expected numeric literal for attribute index");
                                e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                                diagnostics->addError(std::move(e));
                                return nullptr;
                            }
                            attr_index = (unsigned)std::stoul(t.str);
                            t = lexer->nextTok();
                            if(t.type != TOK_RPAREN){
                                delete node;
                                auto e = std::make_unique<UnexpectedToken>("Expected `)` after attribute index");
                                e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                                diagnostics->addError(std::move(e));
                                return nullptr;
                            }
                            t = lexer->nextTok();
                        }
                        funcDecl->params.push_back({var_ty, var_id, attr_name, attr_index, paramAccess, paramConst});
                    } else {
                        funcDecl->params.push_back({var_ty, var_id, {}, {}, paramAccess, paramConst});
                    }

                    if (t.type == TOK_COMMA) {
                        t = lexer->nextTok();
                        if (t.type == TOK_RPAREN) {
                            /// Error; Unexpected Token.
                            auto e = std::make_unique<UnexpectedToken>("Unexpected `)` after `,` in parameter list");
                            e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                            diagnostics->addError(std::move(e));
                            return nullptr;
                        }
                    }
                }

                t = lexer->nextTok();
                /// A `;` at this point makes the decl a forward declaration —
                /// only permitted for plain FuncDecl, not for ShaderDecl.
                if (t.type == TOK_SEMICOLON && funcDecl->type == FUNC_DECL) {
                    funcDecl->isForwardDecl = true;
                    node = funcDecl;
                } else {
                    if (t.type != TOK_LBRACE) {
                        auto e = std::make_unique<UnexpectedToken>("Expected `{` to begin function body");
                        e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                        diagnostics->addError(std::move(e));
                        /// Error. Unexpected Token.
                        return nullptr;
                    }

                    BlockParseContext blockParseContext{ast::builtins::global_scope, true};

                    funcDecl->block.reset(parseBlock(t, blockParseContext));

                    if (!funcDecl->block) {
                        return nullptr;
                    }
                    node = funcDecl;
                }
            }
        }
        else if(t.type == TOK_COLON){
            ast::ResourceDecl *resourceDecl = nullptr;
            if(!staticResourceDecl) {
                resourceDecl = new ast::ResourceDecl();
                resourceDecl->type = RESOURCE_DECL;
            }
            else {
                delete node;
                auto e = std::make_unique<UnexpectedToken>("Expected `(` for static resource declaration");
                e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                diagnostics->addError(std::move(e));
                return nullptr;
            }
            resourceDecl->name = id_for_decl;
            resourceDecl->typeExpr = ty_for_decl;
            t = lexer->nextTok();
            if(t.type != TOK_NUM_LITERAL){
                delete resourceDecl;
                auto e = std::make_unique<UnexpectedToken>("Expected numeric literal for register number");
                e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                diagnostics->addError(std::move(e));
                return nullptr;
            }
            resourceDecl->registerNumber = std::stoul(t.str);
            t = lexer->nextTok();
            // Optional resource-arg clause, e.g. `texture2d t : 1 (swizzle=bgra);`.
            // The parsed entries are stored on the AST node verbatim; Sema
            // validates the keys, value strings, and applicability per
            // resource type (see Sema.cpp / RESOURCE_DECL).
            if(t.type == TOK_LPAREN){
                t = lexer->nextTok();
                OmegaCommon::Vector<ResourceArgEntry> args;
                if(!parseResourceArgList(t, args)){
                    delete resourceDecl;
                    return nullptr;
                }
                for(auto & arg : args){
                    if(arg.name == RESOURCE_PROP_SWIZZLE){
                        // Encoding 0=Identity, 1=R, 2=G, 3=B, 4=A, 5=Zero, 6=One.
                        // Sema enforces length / charset / identity normalization;
                        // here we just translate the four characters into the
                        // ResourceDecl::SwizzleDesc encoding so a malformed value
                        // still produces a populated `swizzleDesc` for Sema to
                        // reject with a precise diagnostic.
                        ast::ResourceDecl::SwizzleDesc sw{};
                        const auto & v = arg.valueStr;
                        auto encodeChar = [](char c) -> unsigned char {
                            switch(c){
                                case 'r': case 'R': return 1;
                                case 'g': case 'G': return 2;
                                case 'b': case 'B': return 3;
                                case 'a': case 'A': return 4;
                                case '0':           return 5;
                                case '1':           return 6;
                                default:            return 0; // sentinel; Sema reports
                            }
                        };
                        sw.r = v.size() > 0 ? encodeChar(v[0]) : 0;
                        sw.g = v.size() > 1 ? encodeChar(v[1]) : 0;
                        sw.b = v.size() > 2 ? encodeChar(v[2]) : 0;
                        sw.a = v.size() > 3 ? encodeChar(v[3]) : 0;
                        resourceDecl->swizzleDesc = sw;
                        // Stash the original token on the decl so Sema can
                        // attach diagnostics to the offending column. We
                        // reuse `loc` for this — Sema reads it before adding
                        // its own per-resource location.
                        resourceDecl->loc = ErrorLoc{
                            arg.valueTok.line, arg.valueTok.line,
                            arg.valueTok.colStart, arg.valueTok.colEnd
                        };
                    }
                    else {
                        delete resourceDecl;
                        auto e = std::make_unique<UnexpectedToken>(std::string("Unknown resource argument `") + arg.name + "` on texture declaration");
                        e->loc = ErrorLoc{ arg.nameTok.line, arg.nameTok.line, arg.nameTok.colStart, arg.nameTok.colEnd };
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }
                }
            }
            if(t.type != TOK_SEMICOLON){
                delete resourceDecl;
                auto e = std::make_unique<UnexpectedToken>("Expected semicolon after resource declaration");
                e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                diagnostics->addError(std::move(e));
                return nullptr;
            }
            node = resourceDecl;
        }
        node->scope = ast::builtins::global_scope;
        return node;
    }

    static Tok eofSentinel {TOK_EOF, "", 0, 0, 0};

    Tok &Parser::getTok() {
        if(tokIdx + 1 >= tokenBuffer.size()){
            return eofSentinel;
        }
        return tokenBuffer[++tokIdx];
    }
    Tok & Parser::aheadTok() {
        if(tokIdx + 1 >= tokenBuffer.size()){
            return eofSentinel;
        }
        return tokenBuffer[tokIdx + 1];
    }

    void Parser::collectTokensUntilEndOfStatement(Tok first_tok) {
        tokenBuffer.clear();
        tokenBuffer.push_back(first_tok);
        if(first_tok.str == KW_IF){
            Tok t = lexer->nextTok();
            if(t.type != TOK_LPAREN){ tokenBuffer.push_back(t); return; }
            tokenBuffer.push_back(t);
            int depth = 1;
            while(depth > 0){
                t = lexer->nextTok();
                tokenBuffer.push_back(t);
                if(t.type == TOK_LPAREN) depth++;
                else if(t.type == TOK_RPAREN) depth--;
            }
            t = lexer->nextTok();
            if(t.type != TOK_LBRACE){ tokenBuffer.push_back(t); return; }
            tokenBuffer.push_back(t);
            depth = 1;
            while(depth > 0){
                t = lexer->nextTok();
                tokenBuffer.push_back(t);
                if(t.type == TOK_LBRACE) depth++;
                else if(t.type == TOK_RBRACE) depth--;
            }
            t = lexer->nextTok();
            while(t.type == TOK_KW && t.str == KW_ELSE){
                tokenBuffer.push_back(t);
                t = lexer->nextTok();
                if(t.type == TOK_KW && t.str == KW_IF){
                    tokenBuffer.push_back(t);
                    t = lexer->nextTok();
                    if(t.type != TOK_LPAREN){ tokenBuffer.push_back(t); return; }
                    tokenBuffer.push_back(t);
                    depth = 1;
                    while(depth > 0){
                        t = lexer->nextTok();
                        tokenBuffer.push_back(t);
                        if(t.type == TOK_LPAREN) depth++;
                        else if(t.type == TOK_RPAREN) depth--;
                    }
                    t = lexer->nextTok();
                    if(t.type != TOK_LBRACE){ tokenBuffer.push_back(t); return; }
                    tokenBuffer.push_back(t);
                    depth = 1;
                    while(depth > 0){
                        t = lexer->nextTok();
                        tokenBuffer.push_back(t);
                        if(t.type == TOK_LBRACE) depth++;
                        else if(t.type == TOK_RBRACE) depth--;
                    }
                    t = lexer->nextTok();
                }
                else if(t.type == TOK_LBRACE){
                    tokenBuffer.push_back(t);
                    depth = 1;
                    while(depth > 0){
                        t = lexer->nextTok();
                        tokenBuffer.push_back(t);
                        if(t.type == TOK_LBRACE) depth++;
                        else if(t.type == TOK_RBRACE) depth--;
                    }
                    t = lexer->nextTok();
                }
                else { tokenBuffer.push_back(t); return; }
            }
            /// t is the token after the last else block — put it back for the next statement.
            putbackTok = t;
        }
        else if(first_tok.str == KW_FOR){
            Tok t = lexer->nextTok();
            if(t.type != TOK_LPAREN){ tokenBuffer.push_back(t); return; }
            tokenBuffer.push_back(t);
            int depth = 1;
            while(depth > 0){
                t = lexer->nextTok();
                tokenBuffer.push_back(t);
                if(t.type == TOK_LPAREN) depth++;
                else if(t.type == TOK_RPAREN) depth--;
            }
            t = lexer->nextTok();
            if(t.type != TOK_LBRACE){ tokenBuffer.push_back(t); return; }
            tokenBuffer.push_back(t);
            depth = 1;
            while(depth > 0){
                t = lexer->nextTok();
                tokenBuffer.push_back(t);
                if(t.type == TOK_LBRACE) depth++;
                else if(t.type == TOK_RBRACE) depth--;
            }
        }
        else if(first_tok.str == KW_WHILE){
            Tok t = lexer->nextTok();
            if(t.type != TOK_LPAREN){ tokenBuffer.push_back(t); return; }
            tokenBuffer.push_back(t);
            int depth = 1;
            while(depth > 0){
                t = lexer->nextTok();
                tokenBuffer.push_back(t);
                if(t.type == TOK_LPAREN) depth++;
                else if(t.type == TOK_RPAREN) depth--;
            }
            t = lexer->nextTok();
            if(t.type != TOK_LBRACE){ tokenBuffer.push_back(t); return; }
            tokenBuffer.push_back(t);
            depth = 1;
            while(depth > 0){
                t = lexer->nextTok();
                tokenBuffer.push_back(t);
                if(t.type == TOK_LBRACE) depth++;
                else if(t.type == TOK_RBRACE) depth--;
            }
        }
        else if(first_tok.str == KW_SWITCH){
            /// `switch(expr) { ... }` — same brace/paren shape as `for`/`while`.
            Tok t = lexer->nextTok();
            if(t.type != TOK_LPAREN){ tokenBuffer.push_back(t); return; }
            tokenBuffer.push_back(t);
            int depth = 1;
            while(depth > 0){
                t = lexer->nextTok();
                tokenBuffer.push_back(t);
                if(t.type == TOK_LPAREN) depth++;
                else if(t.type == TOK_RPAREN) depth--;
            }
            t = lexer->nextTok();
            if(t.type != TOK_LBRACE){ tokenBuffer.push_back(t); return; }
            tokenBuffer.push_back(t);
            depth = 1;
            while(depth > 0){
                t = lexer->nextTok();
                tokenBuffer.push_back(t);
                if(t.type == TOK_LBRACE) depth++;
                else if(t.type == TOK_RBRACE) depth--;
            }
        }
    }

    unsigned Parser::findMatchingParen(unsigned startIdx) {
        int depth = 0;
        for(unsigned i = startIdx; i < tokenBuffer.size(); i++){
            if(tokenBuffer[i].type == TOK_LPAREN) depth++;
            else if(tokenBuffer[i].type == TOK_RPAREN){ depth--; if(depth == 0) return i; }
        }
        return tokenBuffer.size();
    }

    unsigned Parser::findMatchingBrace(unsigned startIdx) {
        int depth = 0;
        for(unsigned i = startIdx; i < tokenBuffer.size(); i++){
            if(tokenBuffer[i].type == TOK_LBRACE) depth++;
            else if(tokenBuffer[i].type == TOK_RBRACE){ depth--; if(depth == 0) return i; }
        }
        return tokenBuffer.size();
    }

    unsigned Parser::findExtentOfStatement(unsigned startIdx) {
        if(startIdx >= tokenBuffer.size()) return startIdx;
        OmegaCommon::StrRef s = tokenBuffer[startIdx].str;
        if(s == KW_IF || s == KW_FOR || s == KW_WHILE){
            if(s == KW_IF){
                unsigned p = startIdx + 1;
                if(p < tokenBuffer.size() && tokenBuffer[p].type == TOK_LPAREN){
                    unsigned closeP = findMatchingParen(p);
                    if(closeP + 1 < tokenBuffer.size() && tokenBuffer[closeP + 1].type == TOK_LBRACE){
                        unsigned closeB = findMatchingBrace(closeP + 1);
                        unsigned i = closeB + 1;
                        while(i < tokenBuffer.size() && tokenBuffer[i].type == TOK_KW && tokenBuffer[i].str == KW_ELSE){
                            i++;
                            if(i < tokenBuffer.size() && tokenBuffer[i].str == KW_IF){
                                i++; if(i >= tokenBuffer.size()) return tokenBuffer.size();
                                if(tokenBuffer[i].type == TOK_LPAREN){
                                    closeP = findMatchingParen(i);
                                    if(closeP + 1 < tokenBuffer.size() && tokenBuffer[closeP + 1].type == TOK_LBRACE){
                                        closeB = findMatchingBrace(closeP + 1);
                                        i = closeB + 1;
                                    }
                                }
                            }
                            else if(i < tokenBuffer.size() && tokenBuffer[i].type == TOK_LBRACE){
                                closeB = findMatchingBrace(i);
                                i = closeB + 1;
                            }
                            else break;
                        }
                        return i - 1;
                    }
                }
            }
            else if(s == KW_FOR || s == KW_WHILE){
                unsigned p = startIdx + 1;
                if(p < tokenBuffer.size() && tokenBuffer[p].type == TOK_LPAREN){
                    unsigned closeP = findMatchingParen(p);
                    if(closeP + 1 < tokenBuffer.size() && tokenBuffer[closeP + 1].type == TOK_LBRACE){
                        unsigned closeB = findMatchingBrace(closeP + 1);
                        return closeB;
                    }
                }
            }
        }
        if(s == KW_SWITCH){
            /// `switch(expr) { ... }` — extent ends at the matching `}`,
            /// same shape as `for` / `while`.
            unsigned p = startIdx + 1;
            if(p < tokenBuffer.size() && tokenBuffer[p].type == TOK_LPAREN){
                unsigned closeP = findMatchingParen(p);
                if(closeP + 1 < tokenBuffer.size() && tokenBuffer[closeP + 1].type == TOK_LBRACE){
                    unsigned closeB = findMatchingBrace(closeP + 1);
                    return closeB;
                }
            }
        }
        for(unsigned i = startIdx; i < tokenBuffer.size(); i++){
            if(tokenBuffer[i].type == TOK_SEMICOLON) return i;
        }
        return tokenBuffer.size() - 1;
    }

    ast::Block *Parser::parseBlockBodyFromBuffer(unsigned startIdx,unsigned endIdx,BlockParseContext & ctxt) {
        auto *block = new ast::Block();
        std::vector<Tok> savedBuf = tokenBuffer;
        unsigned i = startIdx;
        while(i <= endIdx && i < tokenBuffer.size()){
            unsigned endStmt = findExtentOfStatement(i);
            if(endStmt < i) break;
            std::vector<Tok> slice;
            for(unsigned j = i; j <= endStmt && j < tokenBuffer.size(); j++)
                slice.push_back(tokenBuffer[j]);
            tokenBuffer = slice;
            tokIdx = 0;
            Tok first = tokenBuffer[0];
            ast::Stmt *stmt = parseStmtFromBuffer(ctxt);
            if(!stmt){ delete block; tokenBuffer = savedBuf; return nullptr; }
            block->body.push_back(stmt);
            tokenBuffer = savedBuf;
            i = endStmt + 1;
        }
        return block;
    }

    ast::Stmt *Parser::parseStmtFromBuffer(BlockParseContext & ctxt) {
        if(tokIdx >= tokenBuffer.size()) return nullptr;
        Tok first_tok = tokenBuffer[tokIdx];
        if(first_tok.type == TOK_KW){
            if(first_tok.str == KW_IF) return parseIfStmtFromBuffer(ctxt);
            if(first_tok.str == KW_FOR) return parseForStmtFromBuffer(ctxt);
            if(first_tok.str == KW_WHILE) return parseWhileStmtFromBuffer(ctxt);
            if(first_tok.str == KW_SWITCH) return parseSwitchStmtFromBuffer(ctxt);
            if(first_tok.str == KW_RETURN){
                ++tokIdx;
                auto _decl = new ast::ReturnDecl();
                _decl->type = RETURN_DECL;
                _decl->scope = ctxt.parentScope;
                if(tokIdx >= tokenBuffer.size()){
                    _decl->expr = nullptr;
                    return _decl;
                }
                Tok exprFirst = tokenBuffer[tokIdx];
                if(exprFirst.type == TOK_SEMICOLON || exprFirst.type == TOK_RBRACE){
                    _decl->expr = nullptr;
                    return _decl;
                }
                auto _e = parseExpr(exprFirst,ctxt.parentScope);
                if(!_e){ delete _decl; return nullptr; }
                _decl->expr = _e;
                return _decl;
            }
            /// `break` / `continue` / `discard` are statement keywords with
            /// no payload. Route through `parseGenericDecl` so the buffered
            /// block parser builds the proper stmt nodes instead of falling
            /// through to `parseExpr` (which would fail or silently corrupt).
            if(first_tok.str == KW_BREAK || first_tok.str == KW_CONTINUE ||
               first_tok.str == KW_DISCARD){
                ast::Decl *d = parseGenericDecl(first_tok,ctxt);
                return d;
            }
            /// §3.6 / §6.1 — a leading `const` / `threadgroup` qualifier
            /// introduces a var-decl. `parseGenericDecl` owns the prefix-
            /// qualifier var-decl form, so route there directly. Without
            /// this the keyword falls through both the switch above and the
            /// type-token disambiguator below (which only fires on
            /// TOK_KW_TYPE / TOK_ID), reaching neither a decl parse nor a
            /// clean error — the whole enclosing block body was then
            /// silently dropped from the generated source. Mirrors the
            /// `else if (TOK_KW) isDecl = true` branch in `parseStmt`.
            if(first_tok.str == KW_CONST || first_tok.str == KW_THREADGROUP){
                ast::Decl *d = parseGenericDecl(first_tok,ctxt);
                return d;
            }
        }
        bool isDecl = false;
        if(first_tok.type == TOK_KW_TYPE || first_tok.type == TOK_ID){
            auto ahead = (tokIdx + 1 < tokenBuffer.size()) ? tokenBuffer[tokIdx + 1] : Tok{};
            /// §3.6 — postfix `Type const name` is a declaration; recognize
            /// the trailing `const` the same way `parseStmt` does so it
            /// reaches `parseGenericDecl` rather than `parseExpr`.
            if(ahead.type == TOK_ASTERISK || ahead.type == TOK_ID
               || (ahead.type == TOK_KW && ahead.str == KW_CONST)) isDecl = true;
        }
        if(isDecl){
            ast::Decl *d = parseGenericDecl(first_tok,ctxt);
            return d;
        }
        return parseExpr(first_tok,ctxt.parentScope);
    }

    ast::Stmt *Parser::parseIfStmtFromBuffer(BlockParseContext & ctxt) {
        if(tokIdx >= tokenBuffer.size() || tokenBuffer[tokIdx].str != KW_IF) return nullptr;
        ++tokIdx;
        if(tokIdx >= tokenBuffer.size() || tokenBuffer[tokIdx].type != TOK_LPAREN) return nullptr;
        unsigned condStart = tokIdx + 1;
        unsigned condEnd = findMatchingParen(tokIdx) - 1;
        if(condEnd + 1 < condStart) return nullptr;
        ++tokIdx;
        unsigned braceStart = condEnd + 2;
        if(braceStart >= tokenBuffer.size() || tokenBuffer[braceStart].type != TOK_LBRACE) return nullptr;
        unsigned blockEnd = findMatchingBrace(braceStart) - 1;
        unsigned blockStart = braceStart + 1;

        auto *node = new ast::IfStmt();
        node->type = IF_STMT;
        node->scope = ctxt.parentScope;

        std::vector<Tok> savedBuf = tokenBuffer;
        std::vector<Tok> condSlice;
        for(unsigned j = condStart; j <= condEnd; j++) condSlice.push_back(tokenBuffer[j]);
        tokenBuffer = condSlice;
        tokIdx = 0;
        Tok condFirst = condSlice.empty() ? Tok{} : condSlice[0];
        node->condition = parseExpr(condFirst,ctxt.parentScope);
        tokenBuffer = savedBuf;
        if(!node->condition){ delete node; return nullptr; }

        node->thenBlock = std::unique_ptr<ast::Block>(parseBlockBodyFromBuffer(blockStart,blockEnd,ctxt));
        if(!node->thenBlock){ delete node; return nullptr; }

        unsigned i = blockEnd + 2;
        while(i < tokenBuffer.size() && tokenBuffer[i].type == TOK_KW && tokenBuffer[i].str == KW_ELSE){
            i++;
            if(i >= tokenBuffer.size()) break;
            if(tokenBuffer[i].str == KW_IF){
                i++;
                if(i >= tokenBuffer.size() || tokenBuffer[i].type != TOK_LPAREN) break;
                unsigned cStart = i + 1;
                unsigned cEnd = findMatchingParen(i) - 1;
                i = findMatchingParen(i) + 1;
                if(i >= tokenBuffer.size() || tokenBuffer[i].type != TOK_LBRACE) break;
                unsigned bStart = i + 1;
                unsigned bEnd = findMatchingBrace(i) - 1;
                ast::ElseIfBranch branch;
                std::vector<Tok> cSl;
                for(unsigned j = cStart; j <= cEnd; j++) cSl.push_back(tokenBuffer[j]);
                tokenBuffer = cSl;
                tokIdx = 0;
                Tok cFirst = cSl.empty() ? Tok{} : cSl[0];
                branch.condition = parseExpr(cFirst,ctxt.parentScope);
                tokenBuffer = savedBuf;
                if(!branch.condition) break;
                branch.block = std::unique_ptr<ast::Block>(parseBlockBodyFromBuffer(bStart,bEnd,ctxt));
                if(!branch.block) break;
                node->elseIfs.push_back(std::move(branch));
                i = findMatchingBrace(i) + 1;
            }
            else if(tokenBuffer[i].type == TOK_LBRACE){
                unsigned bEnd = findMatchingBrace(i) - 1;
                unsigned bStart = i + 1;
                node->elseBlock = std::unique_ptr<ast::Block>(parseBlockBodyFromBuffer(bStart,bEnd,ctxt));
                i = findMatchingBrace(i) + 1;
                break;
            }
            else break;
        }

        return node;
    }

    ast::Stmt *Parser::parseForStmtFromBuffer(BlockParseContext & ctxt) {
        if(tokIdx >= tokenBuffer.size() || tokenBuffer[tokIdx].str != KW_FOR) return nullptr;
        ++tokIdx;
        if(tokIdx >= tokenBuffer.size() || tokenBuffer[tokIdx].type != TOK_LPAREN) return nullptr;
        unsigned initEnd = tokIdx + 1;
        int depth = 0;
        while(initEnd < tokenBuffer.size()){
            Tok &t = tokenBuffer[initEnd];
            if(t.type == TOK_LPAREN) depth++;
            else if(t.type == TOK_RPAREN) depth--;
            else if(t.type == TOK_SEMICOLON && depth == 0) break;
            initEnd++;
        }
        unsigned initStart = tokIdx + 1;
        unsigned condStart = initEnd + 1;
        unsigned condEnd = condStart;
        while(condEnd < tokenBuffer.size() && tokenBuffer[condEnd].type != TOK_SEMICOLON) condEnd++;
        if(condEnd > condStart) condEnd--;
        unsigned incStart = condEnd + 2;
        unsigned incEnd = incStart;
        while(incEnd < tokenBuffer.size() && tokenBuffer[incEnd].type != TOK_RPAREN) incEnd++;
        unsigned closeParenIdx = incEnd;
        if(incEnd > incStart) incEnd--;
        unsigned braceStart = closeParenIdx + 1;
        if(braceStart >= tokenBuffer.size() || tokenBuffer[braceStart].type != TOK_LBRACE) return nullptr;
        unsigned blockEnd = findMatchingBrace(braceStart) - 1;
        unsigned blockStart = braceStart + 1;

        auto *node = new ast::ForStmt();
        node->type = FOR_STMT;
        node->scope = ctxt.parentScope;

        std::vector<Tok> savedBuf = tokenBuffer;
        std::vector<Tok> initSlice;
        for(unsigned j = initStart; j < initEnd; j++) initSlice.push_back(tokenBuffer[j]);
        tokenBuffer = initSlice;
        tokIdx = 0;
        Tok initFirst = initSlice.empty() ? Tok{} : initSlice[0];
        if(!initSlice.empty()){
            if(initFirst.type == TOK_KW_TYPE || initFirst.type == TOK_ID){
                auto ahead = (tokIdx + 1 < tokenBuffer.size()) ? tokenBuffer[tokIdx + 1] : Tok{};
                if(ahead.type == TOK_ID){ node->init = parseGenericDecl(initFirst,ctxt); }
                else { node->init = parseExpr(initFirst,ctxt.parentScope); }
            }
            else { node->init = parseExpr(initFirst,ctxt.parentScope); }
        }
        else node->init = nullptr;
        tokenBuffer = savedBuf;

        std::vector<Tok> condSlice;
        for(unsigned j = condStart; j <= condEnd; j++) condSlice.push_back(tokenBuffer[j]);
        tokenBuffer = condSlice;
        tokIdx = 0;
        node->condition = condSlice.empty() ? nullptr : parseExpr(condSlice[0],ctxt.parentScope);
        tokenBuffer = savedBuf;

        std::vector<Tok> incSlice;
        for(unsigned j = incStart; j <= incEnd; j++) incSlice.push_back(tokenBuffer[j]);
        tokenBuffer = incSlice;
        tokIdx = 0;
        node->increment = incSlice.empty() ? nullptr : parseExpr(incSlice[0],ctxt.parentScope);
        tokenBuffer = savedBuf;

        node->body = std::unique_ptr<ast::Block>(parseBlockBodyFromBuffer(blockStart,blockEnd,ctxt));
        return node;
    }

    ast::Stmt *Parser::parseWhileStmtFromBuffer(BlockParseContext & ctxt) {
        if(tokIdx >= tokenBuffer.size() || tokenBuffer[tokIdx].str != KW_WHILE) return nullptr;
        ++tokIdx;
        if(tokIdx >= tokenBuffer.size() || tokenBuffer[tokIdx].type != TOK_LPAREN) return nullptr;
        unsigned condStart = tokIdx + 1;
        unsigned condEnd = findMatchingParen(tokIdx) - 1;
        ++tokIdx;
        unsigned braceStart = condEnd + 2;
        if(braceStart >= tokenBuffer.size() || tokenBuffer[braceStart].type != TOK_LBRACE) return nullptr;
        unsigned blockEnd = findMatchingBrace(braceStart) - 1;
        unsigned blockStart = braceStart + 1;

        auto *node = new ast::WhileStmt();
        node->type = WHILE_STMT;
        node->scope = ctxt.parentScope;

        std::vector<Tok> savedBuf = tokenBuffer;
        std::vector<Tok> condSlice;
        for(unsigned j = condStart; j <= condEnd; j++) condSlice.push_back(tokenBuffer[j]);
        tokenBuffer = condSlice;
        tokIdx = 0;
        Tok whileCondFirst = condSlice.empty() ? Tok{} : condSlice[0];
        node->condition = parseExpr(whileCondFirst,ctxt.parentScope);
        tokenBuffer = savedBuf;
        if(!node->condition){ delete node; return nullptr; }

        node->body = std::unique_ptr<ast::Block>(parseBlockBodyFromBuffer(blockStart,blockEnd,ctxt));
        return node;
    }

    ast::Stmt *Parser::parseSwitchStmtFromBuffer(BlockParseContext & ctxt) {
        if(tokIdx >= tokenBuffer.size() || tokenBuffer[tokIdx].str != KW_SWITCH) return nullptr;
        ++tokIdx;
        if(tokIdx >= tokenBuffer.size() || tokenBuffer[tokIdx].type != TOK_LPAREN) return nullptr;
        unsigned condStart = tokIdx + 1;
        unsigned condEnd = findMatchingParen(tokIdx) - 1;
        if(condEnd + 1 < condStart) return nullptr;
        ++tokIdx;
        unsigned braceStart = condEnd + 2;
        if(braceStart >= tokenBuffer.size() || tokenBuffer[braceStart].type != TOK_LBRACE) return nullptr;
        unsigned blockEnd = findMatchingBrace(braceStart) - 1;
        unsigned blockStart = braceStart + 1;

        auto *node = new ast::SwitchStmt();
        node->type = SWITCH_STMT;
        node->scope = ctxt.parentScope;
        node->loc = ErrorLoc{ tokenBuffer[braceStart].line, tokenBuffer[braceStart].line,
                              tokenBuffer[braceStart].colStart, tokenBuffer[braceStart].colEnd };

        std::vector<Tok> savedBuf = tokenBuffer;

        /// Parse the switch condition from a slice.
        std::vector<Tok> condSlice;
        for(unsigned j = condStart; j <= condEnd; j++) condSlice.push_back(tokenBuffer[j]);
        tokenBuffer = condSlice;
        tokIdx = 0;
        Tok condFirst = condSlice.empty() ? Tok{} : condSlice[0];
        node->condition = parseExpr(condFirst, ctxt.parentScope);
        tokenBuffer = savedBuf;
        if(!node->condition){ delete node; return nullptr; }

        /// First pass: collect the index of every top-level `case` / `default`
        /// keyword inside the switch body so we know each case's extent.
        struct LabelMark {
            unsigned kwIdx;     /// index of the `case` or `default` keyword
            unsigned colonIdx;  /// index of the `:` that closes the label
            bool isDefault;
            unsigned valueStart; /// for `case`: first idx of the value expr
            unsigned valueEnd;   /// for `case`: last idx of the value expr (inclusive)
        };
        std::vector<LabelMark> marks;
        {
            unsigned i = blockStart;
            int depthBrace = 0;
            int depthParen = 0;
            while(i <= blockEnd && i < tokenBuffer.size()){
                const Tok &t = tokenBuffer[i];
                if(t.type == TOK_LBRACE){ depthBrace++; i++; continue; }
                if(t.type == TOK_RBRACE){ depthBrace--; i++; continue; }
                if(t.type == TOK_LPAREN){ depthParen++; i++; continue; }
                if(t.type == TOK_RPAREN){ depthParen--; i++; continue; }
                if(depthBrace == 0 && depthParen == 0 && t.type == TOK_KW){
                    if(t.str == KW_CASE){
                        unsigned vs = i + 1;
                        unsigned j = vs;
                        while(j <= blockEnd && j < tokenBuffer.size()
                              && tokenBuffer[j].type != TOK_COLON) j++;
                        if(j > blockEnd){
                            auto e = std::make_unique<UnexpectedToken>("Expected `:` after `case` value in switch body");
                            e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                            diagnostics->addError(std::move(e));
                            tokenBuffer = savedBuf;
                            delete node;
                            return nullptr;
                        }
                        marks.push_back({i, j, false, vs, j - 1});
                        i = j + 1;
                        continue;
                    }
                    if(t.str == KW_DEFAULT){
                        unsigned j = i + 1;
                        if(j > blockEnd || tokenBuffer[j].type != TOK_COLON){
                            auto e = std::make_unique<UnexpectedToken>("Expected `:` after `default` in switch body");
                            e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                            diagnostics->addError(std::move(e));
                            tokenBuffer = savedBuf;
                            delete node;
                            return nullptr;
                        }
                        marks.push_back({i, j, true, 0, 0});
                        i = j + 1;
                        continue;
                    }
                }
                i++;
            }
        }

        if(marks.empty()){
            auto e = std::make_unique<UnexpectedToken>("`switch` body must contain at least one `case` or `default`");
            e->loc = node->loc.value_or(ErrorLoc{});
            diagnostics->addError(std::move(e));
            tokenBuffer = savedBuf;
            delete node;
            return nullptr;
        }

        /// Reject more than one `default:`.
        bool sawDefault = false;
        for(auto &m : marks){
            if(m.isDefault){
                if(sawDefault){
                    auto e = std::make_unique<UnexpectedToken>("`switch` body has more than one `default:` label");
                    e->loc = ErrorLoc{ tokenBuffer[m.kwIdx].line, tokenBuffer[m.kwIdx].line,
                                       tokenBuffer[m.kwIdx].colStart, tokenBuffer[m.kwIdx].colEnd };
                    diagnostics->addError(std::move(e));
                    tokenBuffer = savedBuf;
                    delete node;
                    return nullptr;
                }
                sawDefault = true;
            }
        }

        /// Second pass: for each label, parse its value (if any) and the
        /// statements that follow until the next label (or `blockEnd`).
        for(size_t mi = 0; mi < marks.size(); ++mi){
            ast::SwitchCase sc;
            sc.value = nullptr;

            if(!marks[mi].isDefault){
                /// Parse the case value as an expression. Sema will require
                /// it to be an integer literal.
                std::vector<Tok> valSlice;
                for(unsigned j = marks[mi].valueStart; j <= marks[mi].valueEnd; j++)
                    valSlice.push_back(savedBuf[j]);
                if(valSlice.empty()){
                    auto e = std::make_unique<UnexpectedToken>("Empty `case` value in switch body");
                    e->loc = ErrorLoc{ savedBuf[marks[mi].kwIdx].line, savedBuf[marks[mi].kwIdx].line,
                                       savedBuf[marks[mi].kwIdx].colStart, savedBuf[marks[mi].kwIdx].colEnd };
                    diagnostics->addError(std::move(e));
                    tokenBuffer = savedBuf;
                    delete node;
                    return nullptr;
                }
                tokenBuffer = valSlice;
                tokIdx = 0;
                Tok vf = valSlice[0];
                sc.value = parseExpr(vf, ctxt.parentScope);
                tokenBuffer = savedBuf;
                if(!sc.value){ delete node; return nullptr; }
            }

            unsigned bodyStart = marks[mi].colonIdx + 1;
            unsigned bodyEnd = (mi + 1 < marks.size()) ? (marks[mi + 1].kwIdx - 1) : blockEnd;
            if(bodyEnd >= bodyStart){
                /// Reuse the buffered block parser to walk the case-body
                /// stmt list; it tolerates the absence of an enclosing `{}`
                /// by walking statement-by-statement until `bodyEnd`.
                auto *innerBlock = parseBlockBodyFromBuffer(bodyStart, bodyEnd, ctxt);
                if(innerBlock){
                    for(auto *s : innerBlock->body) sc.body.push_back(s);
                    delete innerBlock;
                }
            }
            node->cases.push_back(std::move(sc));
        }

        return node;
    }

    ast::Stmt *Parser::parseStmt(Tok &first_tok,BlockParseContext & ctxt) {

        ast::Stmt *stmt = nullptr;
        if(first_tok.type == TOK_KW && (first_tok.str == KW_IF || first_tok.str == KW_FOR || first_tok.str == KW_WHILE || first_tok.str == KW_SWITCH)){
            collectTokensUntilEndOfStatement(first_tok);
            tokIdx = 0;
            first_tok = tokenBuffer.front();
            if(first_tok.str == KW_IF) stmt = parseIfStmtFromBuffer(ctxt);
            else if(first_tok.str == KW_FOR) stmt = parseForStmtFromBuffer(ctxt);
            else if(first_tok.str == KW_WHILE) stmt = parseWhileStmtFromBuffer(ctxt);
            else if(first_tok.str == KW_SWITCH) stmt = parseSwitchStmtFromBuffer(ctxt);
            tokenBuffer.clear();
            return stmt;
        }

        tokIdx = 0;
        /// Get Entire Line of Tokens
        while(first_tok.type != TOK_SEMICOLON){
            tokenBuffer.push_back(first_tok);
            first_tok = lexer->nextTok();
        }

        tokenBuffer.push_back(first_tok);


        first_tok = tokenBuffer.front();

        bool isDecl = false;

        /// @note Checks for TypeRef!
        if(first_tok.type == TOK_KW_TYPE || first_tok.type == TOK_ID){
            auto & ahead_tok = aheadTok();

            /// §3.6 — `Type const name` (postfix const) is a declaration,
            /// not an expression. Without the `const`-keyword case the
            /// disambiguator would route it to `parseExpr` and the type
            /// keyword would be misread as an undeclared identifier.
            if(ahead_tok.type == TOK_ASTERISK || ahead_tok.type == TOK_ID
               || (ahead_tok.type == TOK_KW && ahead_tok.str == KW_CONST)){
                isDecl = true;
            }
        }
        else if(first_tok.type == TOK_KW){
            isDecl = true;
        }

        if(isDecl){
            stmt = parseGenericDecl(first_tok,ctxt);
        }
        else {
            stmt = parseExpr(first_tok,ctxt.parentScope);
        }

        tokenBuffer.clear();
        return stmt;
    }

    ast::Decl *Parser::parseGenericDecl(Tok &first_tok,BlockParseContext & ctxt) {
        ast::Decl *node = nullptr;
        /// §3.6 / §6.1 — optional storage qualifier prefix on a local
        /// declaration (`const` or `threadgroup`). Consume it here and
        /// stamp the resulting VarDecl below; it is only valid in front of
        /// a type-introducing token (TOK_KW_TYPE or a struct identifier),
        /// so any other follow-up is rejected once we know the next token.
        /// The two qualifiers are mutually exclusive — `const threadgroup`
        /// is contradictory (immutable vs. mutable shared storage).
        bool isConst = false;
        bool isThreadgroup = false;
        if(first_tok.type == TOK_KW && (first_tok.str == KW_CONST || first_tok.str == KW_THREADGROUP)){
            isConst = (first_tok.str == KW_CONST);
            isThreadgroup = (first_tok.str == KW_THREADGROUP);
            first_tok = getTok();
            if(first_tok.type == TOK_KW && (first_tok.str == KW_CONST || first_tok.str == KW_THREADGROUP)){
                auto e = std::make_unique<UnexpectedToken>(
                    "`const` and `threadgroup` cannot be combined on a declaration");
                e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                diagnostics->addError(std::move(e));
                return nullptr;
            }
        }
        if(first_tok.type == TOK_KW){
            if(isConst || isThreadgroup){
                auto e = std::make_unique<UnexpectedToken>(
                    std::string("`") + (isConst ? "const" : "threadgroup")
                    + "` may only qualify a variable declaration; got `" + first_tok.str + "`");
                e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                diagnostics->addError(std::move(e));
                return nullptr;
            }
            if(first_tok.str == KW_RETURN){
                auto _decl = new ast::ReturnDecl();
                _decl->type = RETURN_DECL;
                first_tok = getTok();
                if(first_tok.type == TOK_SEMICOLON || first_tok.type == TOK_RBRACE || first_tok.type == TOK_EOF){
                    _decl->expr = nullptr;
                }
                else {
                    auto _e = parseExpr(first_tok,ctxt.parentScope);
                    if(_e == nullptr){
                        delete _decl;
                        return nullptr;
                    }
                    _decl->expr = _e;
                }
                node = _decl;
            }
            else if(first_tok.str == KW_BREAK){
                auto *_stmt = new ast::BreakStmt();
                _stmt->type = BREAK_STMT;
                _stmt->scope = ctxt.parentScope;
                _stmt->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                node = (ast::Decl *)_stmt;
            }
            else if(first_tok.str == KW_CONTINUE){
                auto *_stmt = new ast::ContinueStmt();
                _stmt->type = CONTINUE_STMT;
                _stmt->scope = ctxt.parentScope;
                _stmt->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                node = (ast::Decl *)_stmt;
            }
            else if(first_tok.str == KW_DISCARD){
                auto *_stmt = new ast::DiscardStmt();
                _stmt->type = DISCARD_STMT;
                _stmt->scope = ctxt.parentScope;
                _stmt->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                node = (ast::Decl *)_stmt;
            }
        }
        else {
            /// @note Build TypeRef for VarDecl.
            auto _tok = first_tok;
            first_tok = aheadTok();
            bool type_is_pointer = false;
            if(first_tok.type == TOK_ASTERISK){
                type_is_pointer = true;
                ++tokIdx;
            }
            auto type_for_var_decl = buildTypeRef(_tok,type_is_pointer);
            first_tok = getTok();
            /// §3.6 — postfix `T const name` spelling for locals. Sets the
            /// same `isConst` flag the prefix form does. A repeat
            /// (`const T const name`) or a `threadgroup T const name` combo
            /// is contradictory, mirroring the prefix-side checks above.
            if(first_tok.type == TOK_KW && first_tok.str == KW_CONST){
                if(isConst){
                    auto e = std::make_unique<UnexpectedToken>(
                        "Duplicate `const` on declaration");
                    e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                if(isThreadgroup){
                    auto e = std::make_unique<UnexpectedToken>(
                        "`const` and `threadgroup` cannot be combined on a declaration");
                    e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                isConst = true;
                first_tok = getTok();
            }
            if(first_tok.type != TOK_ID){
                auto e = std::make_unique<UnexpectedToken>("Expected identifier for variable name");
                e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                diagnostics->addError(std::move(e));
                return nullptr;
            }
            auto _decl = new ast::VarDecl();
            _decl->type = VAR_DECL;
            _decl->typeExpr = type_for_var_decl;
            _decl->isConst = isConst;
            _decl->isThreadgroup = isThreadgroup;
            _decl->spec.name = first_tok.str;
            first_tok = aheadTok();
            /// One `[N]` per array dimension, outermost first. A bare
            /// `tile[16][16]` collects `{16, 16}`; backends emit the
            /// suffixes in the same order.
            while(first_tok.type == TOK_LBRACKET){
                ++tokIdx;
                first_tok = getTok();
                if(first_tok.type != TOK_NUM_LITERAL){
                    auto e = std::make_unique<UnexpectedToken>("Expected array size literal");
                    e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                _decl->typeExpr->arrayDims.push_back(static_cast<unsigned>(std::stoul(first_tok.str)));
                first_tok = getTok();
                if(first_tok.type != TOK_RBRACKET){
                    auto e = std::make_unique<UnexpectedToken>("Expected `]`");
                    e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                first_tok = aheadTok();
            }
            if(first_tok.type == TOK_OP && first_tok.str == OP_EQUAL){
                ++tokIdx;
                first_tok = getTok();
                auto _e = parseExpr(first_tok,ctxt.parentScope);
                if(_e == nullptr){
                    return nullptr;
                }
                _decl->spec.initializer = _e;
            }
            else if(first_tok.type == TOK_OP){
                auto e = std::make_unique<UnexpectedToken>(std::string("Unknown operator `") + first_tok.str + "`");
                e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                diagnostics->addError(std::move(e));
                return nullptr;
            }
            /// §3.6 — `const` locals are useless without a value. All three
            /// backends require it, so reject up-front with a clear message
            /// instead of leaning on the downstream compiler's diagnostic.
            if(_decl->isConst && !_decl->spec.initializer.has_value()){
                auto e = std::make_unique<UnexpectedToken>(
                    std::string("`const` local `") + _decl->spec.name + "` must have an initializer");
                e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                diagnostics->addError(std::move(e));
                return nullptr;
            }
            /// §6.1 — thread-group-shared memory is uninitialized storage on
            /// every backend; an initializer has no meaning. Reject it where
            /// the diagnostic points at the OmegaSL source.
            if(_decl->isThreadgroup && _decl->spec.initializer.has_value()){
                auto e = std::make_unique<UnexpectedToken>(
                    std::string("`threadgroup` local `") + _decl->spec.name + "` cannot have an initializer");
                e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                diagnostics->addError(std::move(e));
                return nullptr;
            }
            node = _decl;
        }
        node->scope = ctxt.parentScope;
        return node;
    }

    bool Parser::parseObjectExpr(Tok &first_tok, ast::Expr **expr,ast::Scope *parentScope) {
        bool defaultR = true;
        if(first_tok.type == TOK_ID && (first_tok.str == "true" || first_tok.str == "false")){
            auto _e = new ast::LiteralExpr();
            _e->type = LITERAL_EXPR;
            _e->b_val = (first_tok.str == "true");
            _e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
            *expr = _e;
        }
        else if(first_tok.type == TOK_ID || first_tok.type == TOK_KW_TYPE){
            /// A `TOK_KW_TYPE` in expression position is a functional-style
            /// type constructor / cast (e.g. `float(x)`, `float2(a,b)`,
            /// `float2x2(...)`). We represent it as an `IdExpr` carrying the
            /// type name; `parseArgsExpr` rewrites the subsequent call into
            /// either a `CastExpr` (scalar) or a `make_*` builtin call
            /// (vector/matrix).
            auto _e = new ast::IdExpr();
            _e->type = ID_EXPR;
            _e->id = first_tok.str;
            _e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
            *expr = _e;
        }
        else if(first_tok.type == TOK_STR_LITERAL){
            auto _e = new ast::LiteralExpr();
            _e->type = LITERAL_EXPR;
            _e->str = first_tok.str;
            *expr = _e;
        }
        else if(first_tok.type == TOK_NUM_LITERAL){
            auto _e = new ast::LiteralExpr();
            _e->type = LITERAL_EXPR;
            const auto &s = first_tok.str;
            bool isHex = s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');

            /// Suffix detection. Order matters — multi-char suffixes
            /// (`ul`/`lu`) must be checked before single-char (`u`/`l`)
            /// so we don't shave off only one char.
            auto endsWith = [&](OmegaCommon::StrRef suf) -> bool {
                return s.size() >= suf.size() &&
                       std::equal(s.end() - suf.size(), s.end(), suf.begin());
            };
            size_t suffixLen = 0;
            bool hasFloatSuffix = false;
            bool hasHalfSuffix  = false;
            bool hasUintSuffix  = false;
            bool hasLongSuffix  = false;
            bool hasUlongSuffix = false;
            if(!s.empty()){
                if(endsWith("ul") || endsWith("uL") || endsWith("Ul") || endsWith("UL") ||
                   endsWith("lu") || endsWith("lU") || endsWith("Lu") || endsWith("LU")){
                    hasUlongSuffix = true; suffixLen = 2;
                }
                else if(s.back() == 'u' || s.back() == 'U'){
                    hasUintSuffix = true; suffixLen = 1;
                }
                else if(s.back() == 'l' || s.back() == 'L'){
                    hasLongSuffix = true; suffixLen = 1;
                }
                else if((s.back() == 'h' || s.back() == 'H') && !isHex){
                    hasHalfSuffix = true; suffixLen = 1;
                }
                else if((s.back() == 'f' || s.back() == 'F') && !isHex){
                    hasFloatSuffix = true; suffixLen = 1;
                }
            }

            if(isHex){
                /// Strip `0x` prefix and any single-char unsigned/long
                /// suffix, parse as unsigned 64-bit. Width gets demoted
                /// based on the value range when no suffix is present;
                /// `L`/`UL` suffixes pin the wider type.
                OmegaCommon::String digits = s.substr(2, s.size() - 2 - suffixLen);
                uint64_t v = std::stoull(digits, nullptr, 16);
                if(hasUlongSuffix){
                    _e->ui64_num = v;
                }
                else if(hasLongSuffix){
                    _e->i64_num = static_cast<int64_t>(v);
                }
                else if(hasUintSuffix || v > static_cast<uint64_t>(std::numeric_limits<int>::max())){
                    _e->ui_num = static_cast<unsigned>(v);
                }
                else {
                    _e->i_num = static_cast<int>(v);
                }
            }
            else if(s.find('.') != std::string::npos || hasFloatSuffix || hasHalfSuffix){
                /// Half literals (`1.0h`) reuse f_num — Sema's literal
                /// coercion rules already let a float literal initialize
                /// a `half` slot. The `h`-suffix recognition exists so
                /// the syntax doesn't error; the underlying storage is
                /// the same.
                _e->f_num = std::stof(s);
            }
            else if(hasUlongSuffix){
                _e->ui64_num = std::stoull(s.substr(0, s.size() - suffixLen));
            }
            else if(hasLongSuffix){
                _e->i64_num = static_cast<int64_t>(std::stoll(s.substr(0, s.size() - suffixLen)));
            }
            else if(hasUintSuffix){
                _e->ui_num = static_cast<unsigned>(std::stoul(s.substr(0, s.size() - suffixLen)));
            }
            else {
                _e->i_num = std::stoi(s);
            }
            *expr = _e;
        }
        else if(first_tok.type == TOK_LBRACE){
            auto _e = new ast::ArrayExpr();
            _e->type = ARRAY_EXPR;
            while((first_tok = getTok()).type != TOK_RBRACE){
                auto _child_e = parseExpr(first_tok,parentScope);
                first_tok = aheadTok();
                if(first_tok.type == TOK_COMMA){
                    ++tokIdx;
                }
                _e->elm.push_back(_child_e);
            }
            *expr = _e;
        }
        else {
            auto e = std::make_unique<UnexpectedToken>(std::string("Unexpected token `") + first_tok.str + "`");
            e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
            diagnostics->addError(std::move(e));
            return false;
        }
        (*expr)->scope = parentScope;

        return defaultR;
    }

    bool Parser::parseArgsExpr(Tok &first_tok, ast::Expr **expr,ast::Scope *parentScope) {
        bool defaultR = true;
        ast::Expr *_expr = nullptr;
        defaultR = parseObjectExpr(first_tok,&_expr,parentScope);
        first_tok = aheadTok();
        while(true){
            if(first_tok.type == TOK_LPAREN){
                ++tokIdx;

                auto * _call_expr = new ast::CallExpr();
                _call_expr->type = CALL_EXPR;
                _call_expr->callee = _expr;
                if(_expr && _expr->loc) _call_expr->loc = _expr->loc;

                first_tok = getTok();
                while(first_tok.type != TOK_RPAREN){

                   auto _child_expr = parseExpr(first_tok,parentScope);
                   if(_child_expr == nullptr){
                       return false;
//                       break;
                   }

                   _call_expr->args.push_back(_child_expr);

                   first_tok = getTok();
                   if(first_tok.type == TOK_COMMA){
                       first_tok = getTok();
                   }
                }

                /// Functional-style type constructor / cast rewriting.
                /// If the callee is a type keyword (`int`, `float2`, `float2x2`,
                /// etc.), translate the call so later phases see a form they
                /// already handle:
                ///   - scalar types -> CastExpr (single-arg required)
                ///   - vector/matrix types -> call to the matching `make_*`
                ///     builtin, so the existing Sema/CodeGen pathway is reused.
                if(_call_expr->callee && _call_expr->callee->type == ID_EXPR){
                    auto *calleeId = static_cast<ast::IdExpr *>(_call_expr->callee);
                    const auto &n = calleeId->id;
                    bool isScalarCast = (n == KW_TY_INT || n == KW_TY_UINT ||
                                         n == KW_TY_FLOAT || n == KW_TY_BOOL);
                    bool isVecMatCtor =
                        n == KW_TY_INT2 || n == KW_TY_INT3 || n == KW_TY_INT4 ||
                        n == KW_TY_UINT2 || n == KW_TY_UINT3 || n == KW_TY_UINT4 ||
                        n == KW_TY_FLOAT2 || n == KW_TY_FLOAT3 || n == KW_TY_FLOAT4 ||
                        n == KW_TY_FLOAT2X2 || n == KW_TY_FLOAT2X3 || n == KW_TY_FLOAT2X4 ||
                        n == KW_TY_FLOAT3X2 || n == KW_TY_FLOAT3X3 || n == KW_TY_FLOAT3X4 ||
                        n == KW_TY_FLOAT4X2 || n == KW_TY_FLOAT4X3 || n == KW_TY_FLOAT4X4;
                    /// §12.2 follow-up — integer matrices have no inline
                    /// constructor: their array-of-column-vectors lowering
                    /// has no portable rvalue form (HLSL/MSL can't construct a
                    /// raw array as an expression). Reject with a precise
                    /// diagnostic pointing at the per-column build path.
                    bool isIntMatrixCtor =
                        n == KW_TY_INT2X2 || n == KW_TY_INT2X3 || n == KW_TY_INT2X4 ||
                        n == KW_TY_INT3X2 || n == KW_TY_INT3X3 || n == KW_TY_INT3X4 ||
                        n == KW_TY_INT4X2 || n == KW_TY_INT4X3 || n == KW_TY_INT4X4 ||
                        n == KW_TY_UINT2X2 || n == KW_TY_UINT2X3 || n == KW_TY_UINT2X4 ||
                        n == KW_TY_UINT3X2 || n == KW_TY_UINT3X3 || n == KW_TY_UINT3X4 ||
                        n == KW_TY_UINT4X2 || n == KW_TY_UINT4X3 || n == KW_TY_UINT4X4;

                    if(isIntMatrixCtor){
                        auto e = std::make_unique<TypeError>(
                            "Integer matrices cannot be constructed inline. "
                            "Declare the variable and assign each column "
                            "(e.g. `int4x4 m; m[0] = int4(...);`).");
                        e->loc = _call_expr->loc.value_or(ErrorLoc{});
                        diagnostics->addError(std::move(e));
                        delete _call_expr;
                        return false;
                    }

                    if(isScalarCast){
                        if(_call_expr->args.size() != 1){
                            auto e = std::make_unique<ArgumentCountMismatch>();
                            e->functionName = n;
                            e->expected = 1;
                            e->actual = (unsigned)_call_expr->args.size();
                            e->loc = _call_expr->loc.value_or(ErrorLoc{});
                            diagnostics->addError(std::move(e));
                            delete _call_expr;
                            return false;
                        }
                        auto *castExpr = new ast::CastExpr();
                        castExpr->type = CAST_EXPR;
                        castExpr->targetType = ast::TypeExpr::Create(n);
                        castExpr->expr = _call_expr->args[0];
                        castExpr->scope = _call_expr->scope;
                        castExpr->loc = _call_expr->loc;
                        _call_expr->callee = nullptr;
                        _call_expr->args.clear();
                        delete calleeId;
                        delete _call_expr;
                        _expr = castExpr;
                    }
                    else if(isVecMatCtor){
                        /// Rewrite callee name to the matching builtin `make_*`
                        /// FuncType so Sema/CodeGen don't need to know about
                        /// the short form.
                        calleeId->id = std::string("make_") + n;
                        _expr = _call_expr;
                    }
                    else {
                        _expr = _call_expr;
                    }
                }
                else {
                    _expr = _call_expr;
                }
            }
            else if(first_tok.type == TOK_DOT){
                ++tokIdx;

                auto _member_expr = new ast::MemberExpr();
                _member_expr->type = MEMBER_EXPR;
                _member_expr->lhs = _expr;

                first_tok = getTok();
                if(first_tok.type != TOK_ID){
                    auto e = std::make_unique<UnexpectedToken>("Expected identifier after `.`");
                    e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                    diagnostics->addError(std::move(e));
                    *expr = nullptr;
                    delete _member_expr;
                    return false;
                }

                _member_expr->rhs_id = first_tok.str;
                _expr = _member_expr;
            }
            else if(first_tok.type == TOK_LBRACKET){
                ++tokIdx;

                auto _index_expr = new ast::IndexExpr();
                _index_expr->type = INDEX_EXPR;
                _index_expr->lhs = _expr;
                first_tok = getTok();
                _expr = parseExpr(first_tok,parentScope);
                if(_expr == nullptr){
                    *expr = nullptr;
                    delete _index_expr;
                    return false;
                }
                first_tok = getTok();
                if(first_tok.type != TOK_RBRACKET){
                    auto e = std::make_unique<UnexpectedToken>("Expected `]`");
                    e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                    diagnostics->addError(std::move(e));
                    *expr = nullptr;
                    delete _index_expr;
                    return false;
                }
                _index_expr->idx_expr = _expr;
                _expr = _index_expr;
            }
            else {
                *expr = _expr;
                break;
            }
            first_tok = aheadTok();
            _expr->scope = parentScope;
            *expr = _expr;
        }


        return defaultR;
    }

    bool Parser::parseOpExpr(Tok &first_tok, ast::Expr **expr,ast::Scope *parentScope, int minPrec) {
        bool defaultR = true;
        bool hasPrefixOp = false;

        ast::Expr *_expr = nullptr;

        /// @note Unary Pre-Expr Operator Tokens!

        switch (first_tok.type) {
            case TOK_LPAREN : {
                first_tok = getTok();
                /// Cast syntax `(Type)expr` is only recognized when the token
                /// inside the parens is a built-in type keyword. A bare
                /// identifier like `(i)` is a grouping expression, not a cast —
                /// OmegaSL has no user-defined-type casts, and treating
                /// `(ID)` as a cast misparses every parenthesized variable
                /// used in an expression.
                if(first_tok.type == TOK_KW_TYPE && aheadTok().type == TOK_RPAREN){
                    ast::TypeExpr *targetTy = buildTypeRef(first_tok,false,false,nullptr);
                    if(targetTy){
                        ++tokIdx;
                        first_tok = getTok();
                        /// Parse cast operand at unary precedence (higher than any binary op)
                        /// so that `(uint)fx * y` parses as `((uint)fx) * y`, not `(uint)(fx * y)`.
                        ast::Expr *operand = nullptr;
                        if(!parseOpExpr(first_tok, &operand, parentScope, 100)){
                            return false;
                        }
                        if(operand){
                            auto *castExpr = new ast::CastExpr();
                            castExpr->type = CAST_EXPR;
                            castExpr->targetType = targetTy;
                            castExpr->expr = operand;
                            castExpr->scope = parentScope;
                            _expr = castExpr;
                        }
                    }
                    else {
                        _expr = parseExpr(first_tok,parentScope);
                        first_tok = getTok();
                        if(first_tok.type != TOK_RPAREN){
                            if(_expr) delete _expr;
                            auto e = std::make_unique<UnexpectedToken>("Expected `)`");
                            e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                            diagnostics->addError(std::move(e));
                            return false;
                        }
                    }
                }
                else {
                    _expr = parseExpr(first_tok,parentScope);
                    first_tok = getTok();
                    if(first_tok.type != TOK_RPAREN){
                        if(_expr) delete _expr;
                        auto e = std::make_unique<UnexpectedToken>("Expected `)`");
                        e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                        diagnostics->addError(std::move(e));
                        return false;
                    }
                }
                break;
            }
            case TOK_ASTERISK : {
                hasPrefixOp = true;
                auto _pointer_expr = new ast::PointerExpr();
                _pointer_expr->type = POINTER_EXPR;
                _pointer_expr->ptType = ast::PointerExpr::Dereference;

                first_tok = getTok();
                _expr = parseExpr(first_tok,parentScope);
                if(_expr == nullptr){
                    delete _pointer_expr;
                    return false;
                }

                _pointer_expr->expr = _expr;
                _expr = _pointer_expr;
                break;
            }
            case TOK_AMPERSAND : {
                hasPrefixOp = true;
                auto _pointer_expr = new ast::PointerExpr();
                _pointer_expr->type = POINTER_EXPR;
                _pointer_expr->ptType = ast::PointerExpr::AddressOf;

                first_tok = getTok();
                _expr = parseExpr(first_tok,parentScope);
                if(_expr == nullptr){
                    delete _pointer_expr;
                    return false;
                }

                _pointer_expr->expr = _expr;
                _expr = _pointer_expr;
                break;
            }
            /// Pre-Expr Operators
            case TOK_OP : {
                hasPrefixOp = true;
                OmegaCommon::StrRef op_type = first_tok.str;
                if(op_type != OP_NOT && op_type != OP_MINUS && op_type != OP_PLUSPLUS && op_type != OP_MINUSMINUS && op_type != OP_BITNOT){
                    auto e = std::make_unique<UnexpectedToken>(std::string("Invalid operator `") + std::string(op_type) + "` in this context.");
                    e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                    diagnostics->addError(std::move(e));
                    return false;
                }
                auto _unary_op_expr = new ast::UnaryOpExpr();
                _unary_op_expr->type = UNARY_EXPR;
                _unary_op_expr->isPrefix = true;
                _unary_op_expr->op = op_type;
                _unary_op_expr->scope = parentScope;

                first_tok = getTok();
                _expr = parseExpr(first_tok,parentScope);

                if(_expr == nullptr){
                    return false;
                }
                _unary_op_expr->expr = _expr;

                _expr = _unary_op_expr;
                break;
            }
            default : {
                defaultR = parseArgsExpr(first_tok,&_expr,parentScope);
                break;
            }
        }

        /// @note Post-Expr Operators (Unary or Binary)

        if(!hasPrefixOp) {

            first_tok = aheadTok();

            if((first_tok.type == TOK_OP || first_tok.type == TOK_ASTERISK) &&
               (first_tok.str == OP_MINUSMINUS || first_tok.str == OP_PLUSPLUS)){
                ++tokIdx;
                auto unaryExpr = new ast::UnaryOpExpr();
                unaryExpr->type = UNARY_EXPR;
                unaryExpr->op = first_tok.str;
                unaryExpr->isPrefix = false;
                unaryExpr->expr = _expr;
                _expr = unaryExpr;
            }

            /// Precedence-climbing: parse binary operators with precedence >= minPrec (multiplicative > additive > comparison).
            while(true){
                first_tok = aheadTok();
                int prec = getBinaryPrecedence(first_tok);
                if(prec < 0 || prec < minPrec)
                    break;
                ++tokIdx;
                OmegaCommon::String opStr = getBinaryOpStr(first_tok);
                first_tok = getTok();
                ast::Expr *rhs = nullptr;
                if(!parseOpExpr(first_tok, &rhs, parentScope, prec + 1)){
                    if(rhs) delete rhs;
                    return false;
                }
                auto *binaryExpr = new ast::BinaryExpr();
                binaryExpr->type = BINARY_EXPR;
                binaryExpr->op = opStr;
                binaryExpr->lhs = _expr;
                binaryExpr->rhs = rhs;
                binaryExpr->scope = parentScope;
                _expr = binaryExpr;
            }

        }

        *expr = _expr;
        return defaultR;
    }

    ast::Expr *Parser::parseExpr(Tok &first_tok,ast::Scope *parentScope) {
        ast::Expr *expr = nullptr;
        bool b = parseOpExpr(first_tok,&expr,parentScope);
        if(!b){
            return nullptr;
        }

        /// §3.2 — ternary `cond ? a : b`. Right-associative: the `else`
        /// branch is parsed via a recursive `parseExpr` call so that
        /// `a ? b : c ? d : e` reads as `a ? b : (c ? d : e)`. We sit
        /// above the precedence-climber so any binary expression
        /// (including assignment) can be the condition; both branches
        /// can themselves be assignments / nested ternaries.
        Tok &peek = aheadTok();
        if(peek.type == TOK_QUESTION){
            ++tokIdx;  /// consume `?`

            Tok &thenTok = getTok();
            ast::Expr *thenExpr = parseExpr(thenTok, parentScope);
            if(thenExpr == nullptr){
                delete expr;
                return nullptr;
            }

            Tok &colonTok = aheadTok();
            if(colonTok.type != TOK_COLON){
                auto e = std::make_unique<UnexpectedToken>(
                    std::string("Expected `:` to separate ternary branches, got `") + colonTok.str + "`");
                e->loc = ErrorLoc{ colonTok.line, colonTok.line, colonTok.colStart, colonTok.colEnd };
                diagnostics->addError(std::move(e));
                delete expr;
                delete thenExpr;
                return nullptr;
            }
            ++tokIdx;  /// consume `:`

            Tok &elseTok = getTok();
            ast::Expr *elseExpr = parseExpr(elseTok, parentScope);
            if(elseExpr == nullptr){
                delete expr;
                delete thenExpr;
                return nullptr;
            }

            auto *t_e = new ast::TernaryExpr();
            t_e->type = TERNARY_EXPR;
            t_e->scope = parentScope;
            t_e->condition = expr;
            t_e->thenExpr = thenExpr;
            t_e->elseExpr = elseExpr;
            expr = t_e;
        }

        return expr;
    }

    ast::Block *Parser::parseBlock(Tok & first_tok,BlockParseContext & ctxt) {
        /// First_Tok is equal to LBracket;
        auto *block = new ast::Block();
        auto nextBlockTok = [&]() -> Tok {
            if(putbackTok.has_value()){
                Tok t = putbackTok.value();
                putbackTok.reset();
                return t;
            }
            return lexer->nextTok();
        };
        while((first_tok = nextBlockTok()).type != TOK_RBRACE){
            if(first_tok.type == TOK_EOF) break;
            if(first_tok.type != TOK_SEMICOLON){
                auto stmt = parseStmt(first_tok,ctxt);
                if(!stmt){
                    auto e = std::make_unique<UnexpectedToken>("Failed to parse block statement");
                    e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                    diagnostics->addError(std::move(e));
                    delete block;
                    return nullptr;
                }
                block->body.push_back(stmt);
            }
        }
        putbackTok.reset();
        return block;
    }

    void Parser::parseContext(const ParseContext &ctxt) {

        diagnostics = ctxt.diagnostics;
        sem->setDiagnostics(ctxt.diagnostics);

        auto semContext = std::make_shared<SemContext>();

        sem->setSemContext(semContext);

        lexer->setInputStream(&ctxt.in);
        ast::Decl *decl;
        while((decl = parseGlobalDecl()) != nullptr){
            if(sem->performSemForGlobalDecl(decl)){
                foldConstantsInDecl(decl);
                gen->generateDecl(decl);
                if(!gen->generateInterfaceAndCompileShader(decl)){
                    break;
                }
            }
            else {
                /// Stop at the first failed global decl. Unlike statements in
                /// a function body (where `performSemForBlock` recovers and
                /// surfaces every independent error), a global decl registers
                /// shared state — resources, struct types — that later decls
                /// read. Continuing past a half-registered resource/struct
                /// would let a dependent decl dereference the missing piece
                /// and crash (see invalid_buffer_no_args: the empty-element
                /// buffer leaves `args[0]` out of bounds for the shader that
                /// binds it). Cross-decl error recovery would need dependency
                /// tracking; deferred.
                auto e = std::make_unique<UnexpectedToken>("Failed to evaluate statement");
                diagnostics->addError(std::move(e));
                break;
            }
        }
        lexer->finishTokenizeFromStream();

    }

    Parser::~Parser() = default;

}
