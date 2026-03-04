#include <iostream>
#include <string>
#include <vector>
#include <memory>

#ifndef OMEGASL_ERROR_H
#define OMEGASL_ERROR_H

namespace omegasl {

    struct ErrorLoc {
        unsigned lineStart = 0, lineEnd = 0, colStart = 0, colEnd = 0;
    };

    struct Error {
        ErrorLoc loc;
        virtual void format(std::ostream & out) const = 0;
        virtual ~Error() = default;
    };

    struct TypeError : Error {
        std::string message;
        explicit TypeError(std::string msg) : message(std::move(msg)) {}
        void format(std::ostream & out) const override { out << "type error: " << message; }
    };

    struct UndeclaredIdentifier : Error {
        std::string name;
        explicit UndeclaredIdentifier(std::string n) : name(std::move(n)) {}
        void format(std::ostream & out) const override { out << "undeclared identifier: " << name; }
    };

    struct DuplicateDeclaration : Error {
        std::string name;
        explicit DuplicateDeclaration(std::string n) : name(std::move(n)) {}
        void format(std::ostream & out) const override { out << "duplicate declaration: " << name; }
    };

    struct ArgumentCountMismatch : Error {
        std::string functionName;
        unsigned expected = 0, actual = 0;
        void format(std::ostream & out) const override {
            out << "argument count mismatch for " << functionName << ": expected " << expected << ", got " << actual;
        }
    };

    struct UnexpectedToken : Error {
        std::string message;
        explicit UnexpectedToken(std::string msg) : message(std::move(msg)) {}
        void format(std::ostream & out) const override { out << "unexpected token: " << message; }
    };

    struct InvalidAttribute : Error {
        std::string message;
        explicit InvalidAttribute(std::string msg) : message(std::move(msg)) {}
        void format(std::ostream & out) const override { out << "invalid attribute: " << message; }
    };

    class SourceFile {
        std::string content;
        std::vector<std::ios::pos_type> lineStartPos;
    public:
        void setContent(std::string s) { content = std::move(s); }
        const std::string & getContent() const { return content; }
        void buildLinePosMap();
        void toLine(std::istream & stream, unsigned line) const;
        void toCol(std::istream & stream, unsigned col) const;
        bool getLine(unsigned line, std::string & out) const;
        unsigned getLineCount() const { return (unsigned)lineStartPos.size(); }
    };

    class DiagnosticEngine {
        SourceFile * sourceFile = nullptr;
        std::vector<std::unique_ptr<Error>> errors;
    public:
        static const unsigned kMaxErrorsBeforeStop = 50u;

        void setSourceFile(SourceFile * sf) { sourceFile = sf; }
        void addError(std::unique_ptr<Error> err);
        bool hasErrors() const { return !errors.empty(); }
        unsigned getErrorCount() const { return (unsigned)errors.size(); }
        void report(std::ostream & out) const;
        void generateCodeView(std::ostream & out, const ErrorLoc & loc) const;
    };

}

#endif //OMEGASL_ERROR_H
