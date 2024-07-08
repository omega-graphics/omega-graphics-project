#include "omega-common/format.h"
#include <iostream>
#include <cassert>

namespace OmegaCommon {

    inline int power_int(int & val,unsigned power){
        int result = val;
        for(;power > 0;power--){
            result *= val;
        }
        return result;
    }


    class ScalableInteger {
        std::vector<int> buffer;
    public:
        void addPlace(int val){
            for(auto & d : buffer){
                d *= power_int(d,buffer.size());
            }
            buffer.push_back(val);
        }
        int val(){
            int _v = 0;
            for(auto & d : buffer){
                _v += d;
            }
            return _v;
        };
    };

    class Formatter {
        StrRef fmt;
        std::ostream & out;
    public:
        Formatter(StrRef & fmt, std::ostream & out): fmt(fmt), out(out){};
        void format(ArrayRef<ObjectFormatProviderBase *> & objectFormatProviders){
            std::istringstream in(fmt.data());

            auto getChar = [&](){
                return (char) in.get();
            };

            auto aheadChar = [&](){
                char c = in.get();
                in.seekg(-1,std::ios::cur);
                return c;
            };

            std::ostringstream tempBuffer;

            char c;
            while((c = getChar()) != -1){
                switch (c) {
                    case '@' : {
                        tempBuffer.str("");

                        tempBuffer << c;
                        c = getChar();
                        if(c == '{'){
                            ScalableInteger scalableInteger;
                            for(;(c = getChar()) != '}';){
                                if(!std::isdigit(c)){
                                    std::cout << "FORMATTER ERROR:" << "Character " << c << "is not a digit!" << std::endl;
                                    exit(1);
                                };
                                scalableInteger.addPlace(int(c - 0x30));
                            }

                            int val = scalableInteger.val();

                            // std::cout << "Value:" << val << std::endl;

                            assert(val < objectFormatProviders.size());
                            objectFormatProviders[val]->insertFormattedObject(out);
                        }
                        else {
                            out << tempBuffer.str();
                        }
                        break;
                    }
                    default: {
                        out << c;
                        break;
                    }
                }
            }

        }
        ~Formatter()= default;
    };

    Formatter *createFormatter(StrRef fmt, std::ostream & out){
        return new Formatter(fmt,out);
    };

    void format(Formatter * formatter,ArrayRef<ObjectFormatProviderBase *> objectFormatProviders){
        formatter->format(objectFormatProviders);
    };

    void freeFormatter(Formatter *formatter){
        delete formatter;
    };



}