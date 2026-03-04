#ifndef OMEGASL_PREPROCESSOR_H
#define OMEGASL_PREPROCESSOR_H

#include <string>
#include <map>
#include <vector>

namespace omegasl {

class Preprocessor {
public:
    static const unsigned kMaxIncludeDepth = 10;

    void define(const std::string& name, const std::string& value = "1");
    bool isDefined(const std::string& name) const;
    std::string process(const std::string& source, const std::string& currentPath = "");

private:
    std::map<std::string, std::string> macros_;
    std::string processInternal(const std::string& source, const std::string& currentPath, unsigned includeDepth);
    std::string expandMacros(const std::string& line) const;
};

} // namespace omegasl

#endif
