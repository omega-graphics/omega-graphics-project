#include "Preprocessor.h"
#include <sstream>
#include <fstream>
#include <cctype>
#include <algorithm>

namespace omegasl {

void Preprocessor::define(const std::string& name, const std::string& value) {
    macros_[name] = value;
}

bool Preprocessor::isDefined(const std::string& name) const {
    return macros_.find(name) != macros_.end();
}

std::string Preprocessor::process(const std::string& source, const std::string& currentPath) {
    return processInternal(source, currentPath, 0);
}

std::string Preprocessor::expandMacros(const std::string& line) const {
    std::string out = line;
    for (const auto& p : macros_) {
        if (p.first.empty()) continue;
        std::string needle = p.first;
        size_t pos = 0;
        while ((pos = out.find(needle, pos)) != std::string::npos) {
            bool atStart = (pos == 0) || !std::isalnum(static_cast<unsigned char>(out[pos - 1])) && out[pos - 1] != '_';
            bool atEnd = (pos + needle.size() >= out.size()) || !std::isalnum(static_cast<unsigned char>(out[pos + needle.size()])) && out[pos + needle.size()] != '_';
            if (atStart && atEnd) {
                out.replace(pos, needle.size(), p.second);
                pos += p.second.size();
            } else {
                pos += needle.size();
            }
        }
    }
    return out;
}

std::string Preprocessor::processInternal(const std::string& source, const std::string& currentPath, unsigned includeDepth) {
    if (includeDepth > kMaxIncludeDepth) return source;

    std::ostringstream out;
    std::istringstream in(source);
    std::string line;
    std::vector<bool> skipStack;
    bool skipping = false;

    while (std::getline(in, line)) {
        size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
        if (start >= line.size()) {
            if (!skipping) out << line << "\n";
            continue;
        }
        if (line[start] != '#') {
            if (!skipping) out << expandMacros(line) << "\n";
            continue;
        }

        size_t dirStart = start + 1;
        while (dirStart < line.size() && (line[dirStart] == ' ' || line[dirStart] == '\t')) ++dirStart;
        size_t dirEnd = dirStart;
        while (dirEnd < line.size() && line[dirEnd] != ' ' && line[dirEnd] != '\t') ++dirEnd;
        std::string directive(line.begin() + dirStart, line.begin() + dirEnd);
        size_t argStart = dirEnd;
        while (argStart < line.size() && (line[argStart] == ' ' || line[argStart] == '\t')) ++argStart;

        if (directive == "define") {
            if (skipping) continue;
            size_t nameEnd = argStart;
            while (nameEnd < line.size() && (std::isalnum(static_cast<unsigned char>(line[nameEnd])) || line[nameEnd] == '_')) ++nameEnd;
            std::string name(line.begin() + argStart, line.begin() + nameEnd);
            size_t valueStart = nameEnd;
            while (valueStart < line.size() && (line[valueStart] == ' ' || line[valueStart] == '\t')) ++valueStart;
            std::string value(line.begin() + valueStart, line.end());
            if (!name.empty()) define(name, value);
        }
        else if (directive == "ifdef") {
            std::string name(line.begin() + argStart, line.end());
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            skipStack.push_back(skipping);
            skipping = skipping || !isDefined(name);
        }
        else if (directive == "ifndef") {
            std::string name(line.begin() + argStart, line.end());
            name.erase(0, name.find_first_not_of(" \t"));
            name.erase(name.find_last_not_of(" \t") + 1);
            skipStack.push_back(skipping);
            skipping = skipping || isDefined(name);
        }
        else if (directive == "endif") {
            if (!skipStack.empty()) {
                skipping = skipStack.back();
                skipStack.pop_back();
            }
        }
        else if (directive == "include" && !skipping) {
            size_t q = line.find('"', argStart);
            if (q != std::string::npos) {
                size_t q2 = line.find('"', q + 1);
                if (q2 != std::string::npos) {
                    std::string incPath(line.begin() + q + 1, line.begin() + q2);
                    std::string fullPath = currentPath.empty() ? incPath : (currentPath + "/" + incPath);
                    std::ifstream incFile(fullPath);
                    if (incFile) {
                        std::string incContent((std::istreambuf_iterator<char>(incFile)), std::istreambuf_iterator<char>());
                        incFile.close();
                        size_t slash = fullPath.find_last_of("/\\");
                        std::string incDir = (slash != std::string::npos) ? fullPath.substr(0, slash) : "";
                        out << processInternal(incContent, incDir, includeDepth + 1);
                    }
                }
            }
        }
    }
    return out.str();
}

} // namespace omegasl
