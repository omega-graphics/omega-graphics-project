#include "lexer.h"
#include <cctype>

namespace OmegaWrapGen {

    struct LexerError : public Diagnostic {
        OmegaCommon::String message;
        LexerError(OmegaCommon::StrRef _message): message(_message){

        };
        bool isError() override{
            return true;
        };
        void format(std::ostream &out) override{
            out << "ERROR:" << message.data() << std::endl;
        };
    };

    Lexer::Lexer(DiagnosticBuffer & stream):stream(stream){

    };

    bool isKeyword(OmegaCommon::StrRef v){
        return 
        (v == KW_CLASS) || 
        (v == KW_FUNC) || 
        (v == KW_CONST) || 
        (v == KW_INTERFACE) || 
        (v == KW_NAMESPACE) || 
        (v == KW_HEADER);
    };

    void Lexer::setInputStream(std::istream *is){
        this->is = is;
    };

    Tok Lexer::nextTok(){

        auto getChar = [&](){
            return (char)is->get();
        };

        auto aheadChar = [&]() -> char {
            char ch = is->get();
            is->seekg(-1,std::ios::cur);
            return ch;
        };

        Tok t;
        char buffer[TOK_MAX_LEN];


        char *bufferSt = buffer, *bufferEnd = bufferSt;

        #define PUSH_CHAR(c) \
        *bufferEnd = c;\
        ++bufferEnd;

        #define PUSH_TOK(_t) \
        size_t len = bufferEnd - bufferSt;\
        t.content = {bufferSt,len};\
        t.type = _t;\
        goto finish;

        char c;
        while((c = getChar()) != -1){
            switch (c) {
                case ' ': {

                    break;
                }
                case '\n': {

                    break;
                }
                case '/': {
                    PUSH_CHAR(c)
                    c = aheadChar();
                    if(c == '/'){
                        while((c = getChar()) != '\n') {
                            PUSH_CHAR(c)
                        }
                        PUSH_TOK(TOK_LINECOMMENT)
                    }
                    else {
                       stream.push(new LexerError("Unexpected Token"));
                    };
                    break;
                }
                case '{' : {
                    PUSH_CHAR(c)
                    PUSH_TOK(TOK_LBRACE)
                    break;
                }
                case '}' : {
                    PUSH_CHAR(c)
                    PUSH_TOK(TOK_RBRACE)
                    break;
                }
                case '(' : {
                    PUSH_CHAR(c)
                    PUSH_TOK(TOK_LPAREN)
                    break;
                }
                case ')' : {
                    PUSH_CHAR(c)
                    PUSH_TOK(TOK_RPAREN)
                    break;
                }
                case ':' : {
                    PUSH_CHAR(c)
                    PUSH_TOK(TOK_COLON)
                    break;
                }
                case '*' : {
                    PUSH_CHAR(c)
                    PUSH_TOK(TOK_ASTERISK)
                    break;
                }
                case ',' : {
                    PUSH_CHAR(c)
                    PUSH_TOK(TOK_COMMA)
                    break;
                }
                case '&' : {
                    PUSH_CHAR(c)
                    PUSH_TOK(TOK_AMP)
                    break;
                }
                case '"' : {
                    while((c = getChar()) != '"') {
                        PUSH_CHAR(c)
                    }
                    PUSH_TOK(TOK_STRLITERAL)
                    break;
                }
                default : {
                    if(std::isalnum(c)){
                        PUSH_CHAR(c);
                        c = aheadChar();
                        if(!std::isalnum(c)){
                            PUSH_TOK(TOK_ID)
                        };
                    };
                    break;
                }
            }
        };

        PUSH_CHAR(c);
        t.type = TOK_EOF;
        return t;

    finish:

        if(isKeyword(t.content)){
            t.type = TOK_KW;
        };

        return t;
    };

    void Lexer::finish(){
        is = nullptr;
    };
};