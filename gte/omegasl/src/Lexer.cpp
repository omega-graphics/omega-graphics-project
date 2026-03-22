#include "Lexer.h"

#include <cctype>


namespace omegasl {

    inline bool isIdentifierChar(const char &c){
        return std::isalnum(c) || c == '_';
    }

    inline bool isKeyword(OmegaCommon::StrRef subject){
        return (subject == KW_VERTEX) ||
        (subject == KW_FRAGMENT) ||
        (subject == KW_COMPUTE) ||
        (subject == KW_HULL) ||
        (subject == KW_DOMAIN) ||
        (subject == KW_IF) ||
        (subject == KW_ELSE) ||
        (subject == KW_FOR) ||
        (subject == KW_WHILE) ||
        (subject == KW_STRUCT) ||
        (subject == KW_INTERNAL) ||
        (subject == KW_RETURN) ||
        (subject == KW_STATIC);
        /// Note: KW_IN, KW_OUT, KW_INOUT are contextual keywords —
        /// only treated as keywords inside resource maps ([in x, out y]).
        /// Everywhere else they are valid identifiers (e.g. local variable names).
    }

    inline bool isKeywordType(OmegaCommon::StrRef subject){
        return (subject == KW_TY_VOID) ||
        (subject == KW_TY_BOOL) ||
        (subject == KW_TY_INT) ||
        (subject == KW_TY_INT2) ||
        (subject == KW_TY_INT3) ||
        (subject == KW_TY_INT4) ||
        (subject == KW_TY_UINT) ||
        (subject == KW_TY_UINT2) ||
        (subject == KW_TY_UINT3) ||
        (subject == KW_TY_UINT4) ||
        (subject == KW_TY_FLOAT) ||
        (subject == KW_TY_FLOAT2) ||
        (subject == KW_TY_FLOAT3) ||
        (subject == KW_TY_FLOAT4) ||
        (subject == KW_TY_FLOAT2X2) ||
        (subject == KW_TY_FLOAT3X3) ||
        (subject == KW_TY_FLOAT4X4) ||
        (subject == KW_TY_DOUBLE) ||
        (subject == KW_TY_DOUBLE2) ||
        (subject == KW_TY_DOUBLE3) ||
        (subject == KW_TY_DOUBLE4) ||
        (subject == KW_TY_BUFFER) ||
        (subject == KW_TY_TEXTURE1D) ||
        (subject == KW_TY_TEXTURE2D) ||
        (subject == KW_TY_TEXTURE3D) ||
        (subject == KW_TY_SAMPLER1D) ||
        (subject == KW_TY_SAMPLER2D) ||
        (subject == KW_TY_SAMPLER3D);
    }


    void Lexer::setInputStream(std::istream *_in) {
        this->in = _in;
    }


    char nextChar(std::istream * in){
        return (char)in->get();
    }

    char aheadChar(std::istream * in){
        auto c = (char)in->get();
        in->seekg(-1,std::ios::cur);
        return c;
    }

    char Lexer::advanceChar() {
        char c = nextChar(this->in);
        if (c == '\n') {
            currentLine++;
            currentCol = 0;
        } else if (c != EOF) {
            currentCol++;
        }
        return c;
    }

    Tok Lexer::nextTok() {
        tokenStartLine = currentLine;
        tokenStartCol = currentCol;

        char *c_buffer_st = c_buffer, *c_buffer_end = c_buffer_st;


#define NEXT_CHAR() advanceChar()

#define PUSH_CHAR(c) *c_buffer_end = c; ++c_buffer_end

#define AHEAD_CHAR() aheadChar(this->in)

#define SEEK_TO_NEXT_CHAR() do { in->seekg(1,std::ios::cur); currentCol++; } while(0)

#define PUSH_TOK(t) do { \
    auto s = OmegaCommon::String(c_buffer_st,c_buffer_end); \
    Tok _tok; _tok.str = s; _tok.line = tokenStartLine; _tok.colStart = tokenStartCol; _tok.colEnd = currentCol; \
    if(isKeyword(s)) _tok.type = TOK_KW; else if(isKeywordType(s)) _tok.type = TOK_KW_TYPE; else _tok.type = (t); \
    return _tok; } while(0)


        char c = NEXT_CHAR();
        if(c != EOF){
            switch (c) {
                case ' ' : {
                    return nextTok();
                }
                case '\t' : {
                    return nextTok();
                }
                case '\n' : {
                    return nextTok();
                }
                case '/' : {
                    c = AHEAD_CHAR();
                    if(c == '/'){
                        SEEK_TO_NEXT_CHAR();
                        while((c = NEXT_CHAR()) != '\n' && c != EOF){
                            PUSH_CHAR(c);
                        }
                        /// Consume Line Comments
                        return nextTok();
                    }
                    else if(c == '*'){
                        SEEK_TO_NEXT_CHAR();
                        /// Consume Block Comments
                        while(true){
                            c = NEXT_CHAR();
                            if(c == EOF){
                                break;
                            }
                            if(c == '*'){
                                c = AHEAD_CHAR();
                                if(c == '/'){
                                    SEEK_TO_NEXT_CHAR();
                                    break;
                                }
                            }
                            else {
                                PUSH_CHAR(c);
                            }
                        }
                        return nextTok();
                    }
                    PUSH_CHAR('/');
                    if(c == '='){
                        c = NEXT_CHAR();
                        PUSH_CHAR(c);
                    }
                    PUSH_TOK(TOK_OP);
                }
                case '"' : {
                    while((c = NEXT_CHAR()) != '"'){
                        PUSH_CHAR(c);
                    }
                    PUSH_TOK(TOK_STR_LITERAL);
                }

                case '=' : {
                    PUSH_CHAR(c);
                    c = AHEAD_CHAR();
                    if(c == '='){
                        PUSH_CHAR(c);
                        SEEK_TO_NEXT_CHAR();
                    }
                    PUSH_TOK(TOK_OP);
                }

                case '+' : {
                    PUSH_CHAR(c);
                    c = AHEAD_CHAR();
                    if(c == '=' || c == '+'){
                        PUSH_CHAR(c);
                        SEEK_TO_NEXT_CHAR();
                    }
                    PUSH_TOK(TOK_OP);
                }

                case '-' : {
                    PUSH_CHAR(c);
                    c = AHEAD_CHAR();
                    if(c == '=' || c == '-'){
                        PUSH_CHAR(c);
                        SEEK_TO_NEXT_CHAR();
                    }
                    PUSH_TOK(TOK_OP);
                }

                case '>' : {
                    PUSH_CHAR(c);
                    c = AHEAD_CHAR();
                    if(c == '='){
                        PUSH_CHAR(c);
                        SEEK_TO_NEXT_CHAR();
                    }
                    PUSH_TOK(TOK_OP);
                }

                case '<' : {
                    PUSH_CHAR(c);
                    c = AHEAD_CHAR();
                    if(c == '='){
                        PUSH_CHAR(c);
                        SEEK_TO_NEXT_CHAR();
                    }
                    PUSH_TOK(TOK_OP);
                }

                case '!' : {
                    PUSH_CHAR(c);
                    c = AHEAD_CHAR();
                    if(c == '='){
                        PUSH_CHAR(c);
                        SEEK_TO_NEXT_CHAR();
                    }
                    PUSH_TOK(TOK_OP);
                }

                case '*' : {
                    PUSH_CHAR(c);
                    c = AHEAD_CHAR();
                    if(c == '='){
                        PUSH_CHAR(c);
                        SEEK_TO_NEXT_CHAR();
                        PUSH_TOK(TOK_OP);
                    }
                    PUSH_TOK(TOK_ASTERISK);
                }
                case '.' : {
                    PUSH_CHAR(c);
                    PUSH_TOK(TOK_DOT);
                }
                case ',' : {
                    PUSH_CHAR(c);
                    PUSH_TOK(TOK_COMMA);
                }
                case '{' : {
                    PUSH_CHAR(c);
                    PUSH_TOK(TOK_LBRACE);
                }
                case '}' : {
                    PUSH_CHAR(c);
                    PUSH_TOK(TOK_RBRACE);
                }
                case '[' : {
                    PUSH_CHAR(c);
                    PUSH_TOK(TOK_LBRACKET);
                }
                case ']' : {
                    PUSH_CHAR(c);
                    PUSH_TOK(TOK_RBRACKET);
                }
                case '(' : {
                    PUSH_CHAR(c);
                    PUSH_TOK(TOK_LPAREN);
                }
                case ')' : {
                    PUSH_CHAR(c);
                    PUSH_TOK(TOK_RPAREN);
                }
                case '&' : {
                    PUSH_CHAR(c);
                    PUSH_TOK(TOK_AMPERSAND);
                }
                case ':' : {
                    PUSH_CHAR(c);
                    PUSH_TOK(TOK_COLON);
                }
                case ';' : {
                    PUSH_CHAR(c);
                    PUSH_TOK(TOK_SEMICOLON);
                }
                default : {
                    /// If Tok starts with digit
                    if(std::isdigit(c)){
                        PUSH_CHAR(c);
                        bool hasDecimalPoint = false;
                        while(true){
                            c = AHEAD_CHAR();
                            if(std::isdigit(c)){
                                c = NEXT_CHAR();
                                PUSH_CHAR(c);
                                continue;
                            }
                            if(!hasDecimalPoint && c == '.'){
                                hasDecimalPoint = true;
                                c = NEXT_CHAR();
                                PUSH_CHAR(c);
                                continue;
                            }
                            break;
                        }
                        c = AHEAD_CHAR();
                        if(c == 'f' || c == 'F'){
                            c = NEXT_CHAR();
                            PUSH_CHAR(c);
                        }
                        PUSH_TOK(TOK_NUM_LITERAL);
                    }
                    else if(isIdentifierChar(c)){
                        PUSH_CHAR(c);
                        c = AHEAD_CHAR();
                        while(isIdentifierChar(c)){
                            PUSH_CHAR(c);
                            SEEK_TO_NEXT_CHAR();
                            c = AHEAD_CHAR();
                        }
                        PUSH_TOK(TOK_ID);
                    }
                    break;
                }
            }
        }

        PUSH_TOK(TOK_EOF);
    }

    void Lexer::finishTokenizeFromStream() {
        this->in = nullptr;
    }


}
