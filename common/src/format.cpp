#include "omega-common/format.h"
#include <cassert>
#include <cctype>

namespace OmegaCommon {

    class Formatter {
        OmegaCommon::String fmt;
        std::ostream *out;
    public:
        explicit Formatter(StrRef fmt, std::ostream &out):
        fmt(fmt.data(),fmt.size()),
        out(&out){
        }

        const OmegaCommon::String & getFormat() const{
            return fmt;
        }

        std::ostream & getOutput() const{
            return *out;
        }
    };

    Formatter *createFormatter(StrRef fmt, std::ostream & out){
        return new Formatter(fmt,out);
    }

    void format(Formatter * formatter,ArrayRef<ObjectFormatProviderBase *> objectFormatProviders){
        assert(formatter != nullptr && "Formatter must not be null");

        auto &out = formatter->getOutput();
        auto &fmt = formatter->getFormat();

        auto emitPlaceholder = [&](unsigned idx){
            assert(idx < objectFormatProviders.size() && "Template index exceeds argument count");
            if(idx < objectFormatProviders.size()){
                objectFormatProviders.begin()[idx]->insertFormattedObject(out);
            }
        };

        size_t i = 0;
        while(i < fmt.size()){
            char c = fmt[i];
            if(c != '@'){
                out << c;
                ++i;
                continue;
            }

            if((i + 1) >= fmt.size()){
                out << c;
                ++i;
                continue;
            }

            char next = fmt[i + 1];
            if(next == '@'){
                out << '@';
                i += 2;
                continue;
            }

            if(next == '{'){
                size_t j = i + 2;
                if(j >= fmt.size() || !std::isdigit(static_cast<unsigned char>(fmt[j]))){
                    out << '@';
                    ++i;
                    continue;
                }

                unsigned idx = 0;
                while(j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j]))){
                    idx = (idx * 10u) + unsigned(fmt[j] - '0');
                    ++j;
                }

                if(j < fmt.size() && fmt[j] == '}'){
                    emitPlaceholder(idx);
                    i = j + 1;
                    continue;
                }

                out << '@';
                ++i;
                continue;
            }

            if(std::isdigit(static_cast<unsigned char>(next))){
                emitPlaceholder(unsigned(next - '0'));
                i += 2;
                continue;
            }

            out << '@';
            ++i;
        }
    }

    void freeFormatter(Formatter *formatter){
        delete formatter;
    }

} // namespace OmegaCommon
