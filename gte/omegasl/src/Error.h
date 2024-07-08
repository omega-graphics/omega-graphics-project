#include <iostream>
#include "Lexer.h"

#ifndef OMEGASL_ERROR_H
#define OMEGASL_ERROR_H

namespace omegasl {

    struct ErrorLoc {
        unsigned lineStart, lineEnd, colStart,colEnd;
    };

    struct Error {
        ErrorLoc loc;
        virtual void format(std::ostream & out) = 0;
    };


    class SourceFile {
        std::map<unsigned int,std::ios::pos_type> linePosMap;
    public:
        std::stringstream code;
        void buildLinePosMap();
        void toLine(unsigned line);
        void toCol(unsigned col);
    };

    class DiagnosticEngine {
        void generateCodeView(std::ostream & out,SourceFile & sourceFile,ErrorLoc &loc);
    };


}

#endif //OMEGASL_ERROR_H