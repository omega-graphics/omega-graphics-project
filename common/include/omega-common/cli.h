#include "utils.h"

#ifndef OMEGA_COMMON_COMMON_CLI_H
#define OMEGA_COMMON_COMMON_CLI_H


    namespace OmegaCommon::Argv {

        struct OMEGACOMMON_EXPORT ParseError {
            size_t argumentIndex = 0;
            String argument;
            String message;

            String toString() const;
        };

        class OMEGACOMMON_EXPORT Parser {
            enum class OptionMode : int {
                Flag,
                SingleValue,
                MultiValue,
            };

            struct OptionBinding {
                String longName;
                String shortName;
                String valueName;
                String description;
                OptionMode mode = OptionMode::Flag;
                bool * flagValue = nullptr;
                String * singleValue = nullptr;
                Vector<String> * multiValue = nullptr;
            };

            struct PositionalBinding {
                String name;
                String description;
                bool required = true;
                bool multiValue = false;
                String * singleValue = nullptr;
                Vector<String> * multiValueTarget = nullptr;
            };

            String programName;
            String description;
            String usage;
            Vector<OptionBinding> options;
            Vector<PositionalBinding> positionals;

        public:
            Parser() = default;
            explicit Parser(StrRef programName);

            void setProgramName(StrRef name);
            void setDescription(StrRef text);
            void setUsage(StrRef text);

            void addFlag(bool & target, StrRef longName, StrRef shortName = {}, StrRef description = {});
            void addOption(String & target, StrRef longName, StrRef shortName = {}, StrRef valueName = "value", StrRef description = {});
            void addMultiOption(Vector<String> & target, StrRef longName, StrRef shortName = {}, StrRef valueName = "value", StrRef description = {});
            void addPositional(String & target, StrRef name, StrRef description = {}, bool required = true);
            void addPositional(Vector<String> & target, StrRef name, StrRef description = {}, bool required = true);

            bool parse(int argc, char * const argv[], ParseError * error = nullptr);
            bool parse(int argc, const char * const argv[], ParseError * error = nullptr);
            void printHelp(std::ostream & out) const;

        private:
            bool parseArgs(int argc, const char * const argv[], ParseError * error);
            void setError(ParseError * error, size_t argumentIndex, StrRef argument, StrRef message) const;
            const OptionBinding * findLongOption(StrRef name) const;
            const OptionBinding * findShortOption(StrRef name) const;
        };

    } // namespace OmegaCommon::Argv


#endif // OMEGA_COMMON_COMMON_CLI_H
