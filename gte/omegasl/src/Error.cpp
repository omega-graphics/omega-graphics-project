#include "Error.h"
#include <sstream>

namespace omegasl {

    void SourceFile::buildLinePosMap() {
        lineStartPos.clear();
        lineStartPos.push_back(0);
        for (size_t i = 0; i < content.size(); i++) {
            if (content[i] == '\n') {
                lineStartPos.push_back((std::ios::pos_type)(i + 1));
            }
        }
    }

    void SourceFile::toLine(std::istream & stream, unsigned line) const {
        if (line >= 1 && line <= lineStartPos.size()) {
            stream.seekg(lineStartPos[line - 1], std::ios::beg);
        }
    }

    void SourceFile::toCol(std::istream & stream, unsigned col) const {
        auto p = stream.tellg();
        stream.seekg(p + (std::streamoff)col, std::ios::beg);
    }

    bool SourceFile::getLine(unsigned line, std::string & out) const {
        if (line < 1 || line > lineStartPos.size()) return false;
        size_t start = (size_t)lineStartPos[line - 1];
        size_t end = (line < lineStartPos.size()) ? (size_t)lineStartPos[line] : content.size();
        if (end > content.size()) end = content.size();
        if (start < end && content[end - 1] == '\n') end--;
        out = content.substr(start, end - start);
        return true;
    }

    void DiagnosticEngine::addError(std::unique_ptr<Error> err) {
        if (errors.size() < kMaxErrorsBeforeStop) {
            errors.push_back(std::move(err));
        }
    }

    void DiagnosticEngine::generateCodeView(std::ostream & out, const ErrorLoc & loc) const {
        if (!sourceFile) return;
        for (unsigned line = loc.lineStart; line <= loc.lineEnd; line++) {
            if (line == 0) continue;
            std::string lineStr;
            if (!sourceFile->getLine(line, lineStr)) continue;
            out << "  " << line << " | " << lineStr << "\n";
            unsigned colA = (line == loc.lineStart) ? loc.colStart : 0;
            unsigned colB = (line == loc.lineEnd) ? loc.colEnd : (unsigned)lineStr.size();
            if (colB > lineStr.size()) colB = (unsigned)lineStr.size();
            out << "    | ";
            for (unsigned i = 0; i < lineStr.size(); i++) {
                if (i >= colA && i < colB)
                    out << "^";
                else
                    out << " ";
            }
            out << "\n";
        }
    }

    void DiagnosticEngine::report(std::ostream & out) const {
        for (size_t i = 0; i < errors.size(); i++) {
            const Error & e = *errors[i];
            out << "error";
            if (e.loc.lineStart > 0) {
                out << " (" << e.loc.lineStart;
                if (e.loc.colStart > 0) out << ":" << e.loc.colStart;
                out << ")";
            }
            out << ": ";
            e.format(out);
            out << "\n";
            if (sourceFile && e.loc.lineStart > 0) {
                generateCodeView(out, e.loc);
            }
        }
    }

}
