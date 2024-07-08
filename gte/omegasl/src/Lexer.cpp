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
        (subject == KW_IF) ||
        (subject == KW_ELSE) ||
        (subject == KW_FOR) ||
        (subject == KW_WHILE) ||
        (subject == KW_STRUCT) ||
        (subject == KW_INTERNAL) ||
        (subject == KW_IN) ||
        (subject == KW_OUT) ||
        (subject == KW_INOUT) ||
        (subject == KW_RETURN) ||
        (subject == KW_STATIC);
    }

    inline bool isKeywordType(OmegaCommon::StrRef subject){
        return (subject == KW_TY_VOID) ||
        (subject == KW_TY_FLOAT) ||
        (subject == KW_TY_FLOAT2) ||
        (subject == KW_TY_FLOAT3) ||
        (subject == KW_TY_FLOAT4) ||
        (subject == KW_TY_INT) ||
        (subject == KW_TY_UINT);
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

    Tok Lexer::nextTok() {
        char *c_buffer_st = c_buffer, *c_buffer_end = c_buffer_st;


#define NEXT_CHAR() nextChar(this->in)

#define PUSH_CHAR(c) *c_buffer_end = c; ++c_buffer_end

#define AHEAD_CHAR() aheadChar(this->in)

#define SEEK_TO_NEXT_CHAR() in->seekg(1,std::ios::cur)

#define PUSH_TOK(t) auto s = OmegaCommon::String(c_buffer_st,c_buffer_end);   \
if(isKeyword(s)) return {TOK_KW,s};      \
else if(isKeywordType(s)) return {TOK_KW_TYPE,s};   \
else return {t,s};


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
                    c = NEXT_CHAR();
                    if(c == '/'){
                        while((c = NEXT_CHAR()) != '\n'){
                            PUSH_CHAR(c);
                        }
                        /// Consume Line Comments
                        return nextTok();
                    }
                    else if(c == '*'){
                        /// Consume Block Comments
                        while(true){
                            c = NEXT_CHAR();
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
                    else {
                        // Error! Expected / or *.
                    }
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
                    if(c == '='){
                        PUSH_CHAR(c);
                        SEEK_TO_NEXT_CHAR();
                    }
                    PUSH_TOK(TOK_OP);
                }

                case '-' : {
                    PUSH_CHAR(c);
                    c = AHEAD_CHAR();
                    if(c == '='){
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
                        c = AHEAD_CHAR();
                        /// [100].52f
                        while(std::isdigit(c)){
                            c = NEXT_CHAR();
                            PUSH_CHAR(c);
                            c = AHEAD_CHAR();
                        }

                        /// 100[.]52f
                        if(c == '.'){
                            PUSH_CHAR(c);
                            SEEK_TO_NEXT_CHAR();
                        }

                        /// 100.[52]f
                        while(std::isdigit(c)){
                            c = NEXT_CHAR();
                            PUSH_CHAR(c);
                            c = AHEAD_CHAR();
                        }

                        /// 100.52[f]
                        if(c == 'f'){
                            PUSH_CHAR(c);
                            SEEK_TO_NEXT_CHAR();
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