#include "Parser.h"
#include "Sema.h"
#include "ConstFold.h"
#include "CodeGen.h"

namespace omegasl {

    /// Operator precedence for binary expressions (higher = tighter binding). Multiplicative > additive > comparison.
    static int getBinaryPrecedence(const Tok &t) {
        if (t.type == TOK_ASTERISK) return 3;
        if (t.type != TOK_OP) return -1;
        if (t.str == OP_DIV || t.str == "*") return 3;
        if (t.str == OP_PLUS || t.str == OP_MINUS) return 2;
        if (t.str == OP_ISEQUAL || t.str == OP_NOTEQUAL || t.str == OP_LESS || t.str == OP_LESSEQUAL ||
            t.str == OP_GREATER || t.str == OP_GREATEREQUAL) return 1;
        if (t.str == OP_EQUAL || t.str == OP_PLUSEQUAL || t.str == OP_MINUSEQUAL ||
            t.str == OP_MULEQUAL || t.str == OP_DIVEQUAL) return 0;
        return -1;
    }
    static OmegaCommon::String getBinaryOpStr(const Tok &t) {
        if (t.type == TOK_ASTERISK) return "*";
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
                            _decl->fields.push_back({var_ty,var_id,t.str});
                            t = lexer->nextTok();
                        }
                        else {
                            _decl->fields.push_back({var_ty,var_id,{}});
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
            else if(t.str == KW_HULL){
                auto _s = (ast::ShaderDecl *)node;
                _s->shaderType = ast::ShaderDecl::Hull;
            }
            else if(t.str == KW_DOMAIN){
                auto _s = (ast::ShaderDecl *)node;
                _s->shaderType = ast::ShaderDecl::Domain;
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
                if(t.type == TOK_ID){
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
                while(true){

                    OmegaCommon::String sampler_prop_name;
                    OmegaCommon::String sampler_prop_value;
                    unsigned sampler_prop_value_i = 0;

                    if(t.type != TOK_ID){
                        delete res_decl;
                        auto e = std::make_unique<UnexpectedToken>(std::string("Expected identifier for sampler property, got `") + t.str + "`");
                        e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }

                    sampler_prop_name = t.str;

                    t = lexer->nextTok();
                    if(t.type != TOK_OP && t.str != OP_EQUAL){
                        delete res_decl;
                        auto e = std::make_unique<UnexpectedToken>("Expected `=`");
                        e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }

                    t = lexer->nextTok();
                    if(t.type == TOK_ID){
                        sampler_prop_value = t.str;
                    }
                    else if(t.type == TOK_NUM_LITERAL){
                       sampler_prop_value_i = std::stoi(t.str);
                    }
                    else {
                        delete res_decl;
                        auto e = std::make_unique<UnexpectedToken>(std::string("Expected identifier or numeric literal, got `") + t.str + "`");
                        e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }

                    if(sampler_prop_name == SAMPLER_PROP_FILTER){
                        if(sampler_prop_value == SAMPLER_FILTER_LINEAR){
                            samplerDesc.filter = OMEGASL_SHADER_SAMPLER_LINEAR_FILTER;
                        }
                        else if(sampler_prop_value == SAMPLER_FILTER_POINT){
                            samplerDesc.filter = OMEGASL_SHADER_SAMPLER_POINT_FILTER;
                        }
                        else if(sampler_prop_value == SAMPLER_FILTER_ANISOTROPIC){
                            samplerDesc.filter = OMEGASL_SHADER_SAMPLER_MAX_ANISOTROPY_FILTER;
                        }
                    }
                    else if(sampler_prop_name == SAMPLER_PROP_ADDRESS_MODE){
                        omegasl_shader_static_sampler_address_mode addressMode;
                        if(sampler_prop_value == SAMPLER_ADDRESS_MODE_WRAP){
                            addressMode = OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_WRAP;
                        }
                        else if(sampler_prop_value == SAMPLER_ADDRESS_MODE_MIRROR){
                            addressMode = OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRROR;
                        }
                        else if(sampler_prop_value == SAMPLER_ADDRESS_MODE_MIRRORWRAP){
                            addressMode = OMEGASL_SHADER_SAMPLER_ADDRESS_MODE_MIRRORWRAP;
                        }
                        samplerDesc.uAddressMode = samplerDesc.vAddressMode = samplerDesc.wAddressMode = addressMode;
                    }
                    else if(sampler_prop_name == SAMPLER_PROP_MAX_ANISOTROPY){
                        samplerDesc.maxAnisotropy = sampler_prop_value_i;
                    }

                    t = lexer->nextTok();

                    if(t.type == TOK_COMMA){
                        t = lexer->nextTok();
                    }
                    else if(t.type == TOK_RPAREN){
                        break;
                    }
                    else {
                        delete res_decl;
                        auto e = std::make_unique<UnexpectedToken>(std::string("Unexpected token `") + t.str + "`");
                        e->loc = ErrorLoc{ t.line, t.line, t.colStart, t.colEnd };
                        diagnostics->addError(std::move(e));
                        return nullptr;
                    }

                }

                t = lexer->nextTok();
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
                    auto _tok = t;
                    t = lexer->nextTok();
                    bool type_is_pointer = false;

                    if (t.type == TOK_ASTERISK) {
                        type_is_pointer = true;
                        t = lexer->nextTok();
                    }

                    auto var_ty = buildTypeRef(_tok, type_is_pointer);

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

                        funcDecl->params.push_back({var_ty, var_id, t.str});
                        t = lexer->nextTok();
                    } else {
                        funcDecl->params.push_back({var_ty, var_id, {}});
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

    Tok &Parser::getTok() {
        return tokenBuffer[++tokIdx];
    }
    Tok & Parser::aheadTok() {
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
        }
        bool isDecl = false;
        if(first_tok.type == TOK_KW_TYPE || first_tok.type == TOK_ID){
            auto ahead = (tokIdx + 1 < tokenBuffer.size()) ? tokenBuffer[tokIdx + 1] : Tok{};
            if(ahead.type == TOK_ASTERISK || ahead.type == TOK_ID) isDecl = true;
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

    ast::Stmt *Parser::parseStmt(Tok &first_tok,BlockParseContext & ctxt) {

        ast::Stmt *stmt = nullptr;
        if(first_tok.type == TOK_KW && (first_tok.str == KW_IF || first_tok.str == KW_FOR || first_tok.str == KW_WHILE)){
            collectTokensUntilEndOfStatement(first_tok);
            tokIdx = 0;
            first_tok = tokenBuffer.front();
            if(first_tok.str == KW_IF) stmt = parseIfStmtFromBuffer(ctxt);
            else if(first_tok.str == KW_FOR) stmt = parseForStmtFromBuffer(ctxt);
            else if(first_tok.str == KW_WHILE) stmt = parseWhileStmtFromBuffer(ctxt);
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

            if(ahead_tok.type == TOK_ASTERISK || ahead_tok.type == TOK_ID){
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
        if(first_tok.type == TOK_KW){
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
            if(first_tok.type != TOK_ID){
                auto e = std::make_unique<UnexpectedToken>("Expected identifier for variable name");
                e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                diagnostics->addError(std::move(e));
                return nullptr;
            }
            auto _decl = new ast::VarDecl();
            _decl->type = VAR_DECL;
            _decl->typeExpr = type_for_var_decl;
            _decl->spec.name = first_tok.str;
            first_tok = aheadTok();
            if(first_tok.type == TOK_LBRACKET){
                ++tokIdx;
                first_tok = getTok();
                if(first_tok.type != TOK_NUM_LITERAL){
                    auto e = std::make_unique<UnexpectedToken>("Expected array size literal");
                    e->loc = ErrorLoc{ first_tok.line, first_tok.line, first_tok.colStart, first_tok.colEnd };
                    diagnostics->addError(std::move(e));
                    return nullptr;
                }
                _decl->typeExpr->arraySize = static_cast<unsigned>(std::stoul(first_tok.str));
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
        else if(first_tok.type == TOK_ID){
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
            if(s.find('.') != std::string::npos || s.back() == 'f' || s.back() == 'F'){
                _e->f_num = std::stof(s);
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

                _expr = _call_expr;
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
                if((first_tok.type == TOK_KW_TYPE || first_tok.type == TOK_ID) && aheadTok().type == TOK_RPAREN){
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
                if(op_type != OP_NOT && op_type != OP_MINUS && op_type != OP_PLUSPLUS && op_type != OP_MINUSMINUS){
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
                gen->generateInterfaceAndCompileShader(decl);
            }
            else {
                auto e = std::make_unique<UnexpectedToken>("Failed to evaluate statement");
                diagnostics->addError(std::move(e));
                break;
            }
        }
        lexer->finishTokenizeFromStream();

    }

    Parser::~Parser() = default;

}
