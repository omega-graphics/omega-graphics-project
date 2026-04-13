#include "omega-common/cli.h"

#include <cassert>
#include <iomanip>

namespace OmegaCommon {
    namespace Argv {

        namespace {

            String copyRef(StrRef value) {
                if(value.data() == nullptr || value.size() == 0){
                    return {};
                }
                return String(value.data(), value.size());
            }

            StrRef trimLeadingDashes(StrRef value) {
                size_t offset = 0;
                while(offset < value.size() && value[offset] == '-'){
                    ++offset;
                }

                if(value.data() == nullptr){
                    return {};
                }
                return StrRef(value.data() + offset, static_cast<StrRef::size_type>(value.size() - offset));
            }

            String normalizeName(StrRef name) {
                return copyRef(trimLeadingDashes(name));
            }

            size_t findChar(StrRef value, char ch) {
                for(size_t i = 0; i < value.size(); ++i){
                    if(value[i] == ch){
                        return i;
                    }
                }
                return value.size();
            }

        } // namespace

        String ParseError::toString() const {
            if(argument.empty()){
                return message;
            }

            if(message.empty()){
                return argument;
            }

            String out = argument;
            out.append(": ");
            out.append(message);
            return out;
        }

        Parser::Parser(StrRef programName) {
            setProgramName(programName);
        }

        void Parser::setProgramName(StrRef name) {
            programName = copyRef(name);
        }

        void Parser::setDescription(StrRef text) {
            description = copyRef(text);
        }

        void Parser::setUsage(StrRef text) {
            usage = copyRef(text);
        }

        void Parser::addFlag(bool & target, StrRef longName, StrRef shortName, StrRef text) {
            OptionBinding option {};
            option.longName = normalizeName(longName);
            option.shortName = normalizeName(shortName);
            option.description = copyRef(text);
            option.mode = OptionMode::Flag;
            option.flagValue = std::addressof(target);
            assert(!option.longName.empty() || !option.shortName.empty());
            options.push_back(std::move(option));
        }

        void Parser::addOption(String & target, StrRef longName, StrRef shortName, StrRef valueName, StrRef text) {
            OptionBinding option {};
            option.longName = normalizeName(longName);
            option.shortName = normalizeName(shortName);
            option.valueName = copyRef(valueName);
            if(option.valueName.empty()){
                option.valueName = "value";
            }
            option.description = copyRef(text);
            option.mode = OptionMode::SingleValue;
            option.singleValue = std::addressof(target);
            assert(!option.longName.empty() || !option.shortName.empty());
            options.push_back(std::move(option));
        }

        void Parser::addMultiOption(Vector<String> & target, StrRef longName, StrRef shortName, StrRef valueName, StrRef text) {
            OptionBinding option {};
            option.longName = normalizeName(longName);
            option.shortName = normalizeName(shortName);
            option.valueName = copyRef(valueName);
            if(option.valueName.empty()){
                option.valueName = "value";
            }
            option.description = copyRef(text);
            option.mode = OptionMode::MultiValue;
            option.multiValue = std::addressof(target);
            assert(!option.longName.empty() || !option.shortName.empty());
            options.push_back(std::move(option));
        }

        void Parser::addPositional(String & target, StrRef name, StrRef text, bool required) {
            assert(positionals.empty() || !positionals.back().multiValue);

            PositionalBinding binding {};
            binding.name = copyRef(name);
            binding.description = copyRef(text);
            binding.required = required;
            binding.singleValue = std::addressof(target);
            positionals.push_back(std::move(binding));
        }

        void Parser::addPositional(Vector<String> & target, StrRef name, StrRef text, bool required) {
            assert(positionals.empty() || !positionals.back().multiValue);

            PositionalBinding binding {};
            binding.name = copyRef(name);
            binding.description = copyRef(text);
            binding.required = required;
            binding.multiValue = true;
            binding.multiValueTarget = std::addressof(target);
            positionals.push_back(std::move(binding));
        }

        bool Parser::parse(int argc, char * const argv[], ParseError * error) {
            Vector<const char *> args;
            args.reserve(static_cast<size_t>(argc));
            for(int i = 0; i < argc; ++i){
                args.push_back(argv[i]);
            }
            return parseArgs(argc, args.data(), error);
        }

        bool Parser::parse(int argc, const char * const argv[], ParseError * error) {
            return parseArgs(argc, argv, error);
        }

        bool Parser::parseArgs(int argc, const char * const argv[], ParseError * error) {
            if(programName.empty() && argc > 0 && argv != nullptr && argv[0] != nullptr){
                programName = argv[0];
            }

            Vector<bool> positionalAssigned(positionals.size(), false);
            size_t nextPositional = 0;
            bool positionalOnly = false;

            for(int i = 1; i < argc; ++i){
                StrRef arg(argv[i] != nullptr ? argv[i] : "");

                if(!positionalOnly && arg == "--"){
                    positionalOnly = true;
                    continue;
                }

                if(!positionalOnly && arg.size() > 2 && arg[0] == '-' && arg[1] == '-'){
                    StrRef token(arg.data() + 2, static_cast<StrRef::size_type>(arg.size() - 2));
                    auto equalsIndex = findChar(token, '=');
                    bool hasInlineValue = equalsIndex != token.size();
                    StrRef optionName(token.data(), static_cast<StrRef::size_type>(hasInlineValue ? equalsIndex : token.size()));

                    auto option = findLongOption(optionName);
                    if(option == nullptr){
                        setError(error, static_cast<size_t>(i), arg, "unknown option");
                        return false;
                    }

                    if(option->mode == OptionMode::Flag){
                        if(hasInlineValue){
                            setError(error, static_cast<size_t>(i), arg, "flag option does not take a value");
                            return false;
                        }
                        assert(option->flagValue != nullptr);
                        *(option->flagValue) = true;
                        continue;
                    }

                    String optionValue;
                    if(hasInlineValue){
                        optionValue = copyRef(StrRef(token.data() + equalsIndex + 1, static_cast<StrRef::size_type>(token.size() - equalsIndex - 1)));
                    }
                    if(!hasInlineValue){
                        if((i + 1) >= argc){
                            setError(error, static_cast<size_t>(i), arg, "missing value for option");
                            return false;
                        }
                        ++i;
                        optionValue = copyRef(StrRef(argv[i] != nullptr ? argv[i] : ""));
                    }

                    if(option->mode == OptionMode::SingleValue){
                        assert(option->singleValue != nullptr);
                        *(option->singleValue) = std::move(optionValue);
                    }
                    else {
                        assert(option->multiValue != nullptr);
                        option->multiValue->push_back(std::move(optionValue));
                    }
                    continue;
                }

                if(!positionalOnly && arg.size() > 1 && arg[0] == '-'){
                    if(arg.size() != 2){
                        setError(error, static_cast<size_t>(i), arg, "short option bundles are not supported");
                        return false;
                    }

                    StrRef optionName(arg.data() + 1, 1);
                    auto option = findShortOption(optionName);
                    if(option == nullptr){
                        setError(error, static_cast<size_t>(i), arg, "unknown option");
                        return false;
                    }

                    if(option->mode == OptionMode::Flag){
                        assert(option->flagValue != nullptr);
                        *(option->flagValue) = true;
                        continue;
                    }

                    if((i + 1) >= argc){
                        setError(error, static_cast<size_t>(i), arg, "missing value for option");
                        return false;
                    }

                    ++i;
                    String optionValue = copyRef(StrRef(argv[i] != nullptr ? argv[i] : ""));
                    if(option->mode == OptionMode::SingleValue){
                        assert(option->singleValue != nullptr);
                        *(option->singleValue) = std::move(optionValue);
                    }
                    else {
                        assert(option->multiValue != nullptr);
                        option->multiValue->push_back(std::move(optionValue));
                    }
                    continue;
                }

                if(nextPositional >= positionals.size()){
                    setError(error, static_cast<size_t>(i), arg, "unexpected positional argument");
                    return false;
                }

                auto & positional = positionals[nextPositional];
                if(positional.multiValue){
                    assert(positional.multiValueTarget != nullptr);
                    positional.multiValueTarget->push_back(copyRef(arg));
                    positionalAssigned[nextPositional] = true;
                }
                else {
                    assert(positional.singleValue != nullptr);
                    *(positional.singleValue) = copyRef(arg);
                    positionalAssigned[nextPositional] = true;
                    ++nextPositional;
                }
            }

            for(size_t i = 0; i < positionals.size(); ++i){
                const auto & positional = positionals[i];
                if(positional.required && !positionalAssigned[i]){
                    String missingArgument;
                    missingArgument.push_back('<');
                    missingArgument.append(positional.name);
                    if(positional.multiValue){
                        missingArgument.append("...");
                    }
                    missingArgument.push_back('>');
                    setError(error, static_cast<size_t>(argc), missingArgument, "missing required positional argument");
                    return false;
                }
            }

            return true;
        }

        void Parser::printHelp(std::ostream & out) const {
            if(!description.empty()){
                out << description << std::endl << std::endl;
            }

            out << "Usage: ";
            if(programName.empty()){
                out << "program";
            }
            else {
                out << programName;
            }

            if(!usage.empty()){
                out << " " << usage;
            }
            else {
                if(!options.empty()){
                    out << " [options]";
                }
                for(const auto & positional : positionals){
                    out << " ";
                    if(positional.required){
                        out << "<" << positional.name;
                        if(positional.multiValue){
                            out << "...";
                        }
                        out << ">";
                    }
                    else {
                        out << "[" << positional.name;
                        if(positional.multiValue){
                            out << "...";
                        }
                        out << "]";
                    }
                }
            }

            out << std::endl;

            if(!positionals.empty()){
                out << std::endl;
                out << "Positional Arguments:" << std::endl;

                Vector<String> labels;
                labels.reserve(positionals.size());
                size_t width = 0;
                for(const auto & positional : positionals){
                    String label = positional.name;
                    if(positional.multiValue){
                        label.append("...");
                    }
                    width = std::max(width, label.size());
                    labels.push_back(std::move(label));
                }

                for(size_t i = 0; i < positionals.size(); ++i){
                    out << "  " << std::left << std::setw(static_cast<int>(width)) << labels[i];
                    if(!positionals[i].description.empty()){
                        out << "  " << positionals[i].description;
                    }
                    if(!positionals[i].required){
                        out << " (optional)";
                    }
                    out << std::endl;
                }
            }

            if(!options.empty()){
                out << std::endl;
                out << "Options:" << std::endl;

                Vector<String> labels;
                labels.reserve(options.size());
                size_t width = 0;
                for(const auto & option : options){
                    String label;
                    if(!option.shortName.empty()){
                        label.push_back('-');
                        label.append(option.shortName);
                        if(!option.longName.empty()){
                            label.append(", ");
                        }
                    }

                    if(!option.longName.empty()){
                        label.append("--");
                        label.append(option.longName);
                    }

                    if(option.mode != OptionMode::Flag){
                        label.push_back(' ');
                        label.push_back('<');
                        label.append(option.valueName);
                        label.push_back('>');
                    }

                    width = std::max(width, label.size());
                    labels.push_back(std::move(label));
                }

                for(size_t i = 0; i < options.size(); ++i){
                    out << "  " << std::left << std::setw(static_cast<int>(width)) << labels[i];
                    if(!options[i].description.empty()){
                        out << "  " << options[i].description;
                    }
                    out << std::endl;
                }
            }
        }

        void Parser::setError(ParseError * error, size_t argumentIndex, StrRef argument, StrRef message) const {
            if(error == nullptr){
                return;
            }

            error->argumentIndex = argumentIndex;
            error->argument = copyRef(argument);
            error->message = copyRef(message);
        }

        const Parser::OptionBinding * Parser::findLongOption(StrRef name) const {
            auto normalized = normalizeName(name);
            for(const auto & option : options){
                if(option.longName == normalized){
                    return &option;
                }
            }
            return nullptr;
        }

        const Parser::OptionBinding * Parser::findShortOption(StrRef name) const {
            auto normalized = normalizeName(name);
            for(const auto & option : options){
                if(option.shortName == normalized){
                    return &option;
                }
            }
            return nullptr;
        }

    } // namespace Argv
} // namespace OmegaCommon
