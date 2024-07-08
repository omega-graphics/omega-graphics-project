#include "../lexer.h"
#include <iostream>

#include <omega-common/common.h>

using namespace OmegaWrapGen;

int main(int argc,char *argv[]){

    OmegaCommon::StrRef flag {argv[1]};

    std::ifstream in(flag.data());
    DiagnosticBuffer buffer;
    Lexer lex(buffer);
    lex.setInputStream(&in);
    Tok t = lex.nextTok();
    for(;t.type != TOK_EOF && !buffer.hasErrored();t = lex.nextTok()){
        std::cout << "TOK: { '" << t.content << "' T:" << std::hex << t.type << std::dec << "}\n" << std::endl;
    };
    if(buffer.hasErrored())
        buffer.logAll();
    
    lex.finish();

    return 0;
};