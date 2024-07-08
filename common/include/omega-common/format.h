#include "utils.h"
#include <ctime>
#include <tuple>
#include <array>
#include <memory>

#ifndef OMEGA_COMMON_FORMAT_H
#define OMEGA_COMMON_FORMAT_H

namespace OmegaCommon {

    template<typename T>
    struct FormatProvider;

    template<typename T>
    using _has_format_provider = std::is_function<decltype(FormatProvider<T>::format)>;

    struct ObjectFormatProviderBase {
        virtual void insertFormattedObject(std::ostream & os) = 0;
        virtual ~ObjectFormatProviderBase() = default;
    };

    template<class T,class Pr = FormatProvider<T>>
             struct ObjectFormatProvider : public ObjectFormatProviderBase {
        T & object;
        void insertFormattedObject(std::ostream &os) override {
            Pr::format(os,object);
        };

        template<std::enable_if_t<_has_format_provider<T>::value,int> = 0>
                explicit ObjectFormatProvider(T & object):object(object){

        };
        ~ObjectFormatProvider() = default;
    };


    template<>
    struct FormatProvider<int> {
        static void format(std::ostream & os,int & object){
            os << object;
        }
    };

    template<>
    struct FormatProvider<char> {
        static void format(std::ostream & os,char & object){
            os << object;
        }
    };

    template<>
    struct FormatProvider<std::string> {
        static void format(std::ostream & os,std::string & object){
            os << object;
        }
    };

    template<>
    struct FormatProvider<StrRef> {
        static void format(std::ostream & os, StrRef & object){
            os << object.data();
        }
    };

    template<typename T>
    struct FormatProvider<std::shared_ptr<T>>{
         static void format(std::ostream & os,std::shared_ptr<T> & object){
            os << "SharedHandle(0x" << std::hex << object.get() << std::dec << ") : " << std::flush;
            FormatProvider<T>::format(os,*object);
        }
    };

    template<typename T>
    struct FormatProvider {
        template<std::enable_if_t<std::is_array_v<decltype(T::OMEGACOMMON_CLASS_ID)>,int> = 0>
        FormatProvider(){};
        static void format(std::ostream & os,T & object){
            os << "<" << T::OMEGACOMMON_CLASS_ID << ">";
        }
    };

    class Formatter;

    OMEGACOMMON_EXPORT Formatter *createFormatter(StrRef fmt, std::ostream & out);
    OMEGACOMMON_EXPORT void format(Formatter * formatter,ArrayRef<ObjectFormatProviderBase *> objectFormatProviders);
    OMEGACOMMON_EXPORT void freeFormatter(Formatter *formatter);

    template<typename T>
     ObjectFormatProvider<T> * buildFormatProvider(T object){
        return new ObjectFormatProvider<T>(object);
    };

    template<class ..._Args>
     OmegaCommon::String fmtString(const char *fmt,_Args && ...args){
        std::ostringstream out;
//        auto t_args = std::make_tuple(std::forward<_Args>(args)...);
        std::array<ObjectFormatProviderBase *,sizeof...(args)> arrayArgs = {buildFormatProvider(std::forward<_Args>(args))...};
        Formatter * formatter = createFormatter(fmt,out);
        format(formatter,{arrayArgs.data(),arrayArgs.data() + arrayArgs.size()});
        freeFormatter(formatter);
        for(auto a : arrayArgs){
            delete a;
        }
        
        return out.str();
    };

    template<class ..._Args>
    void LogV(const char *fmt,_Args && ...args){
        auto t = std::time(nullptr);
        std::cout << "[" << "LOG" << "] " << fmtString(fmt,std::forward<_Args>(args)...) << std::endl;
    }
    
};

#endif