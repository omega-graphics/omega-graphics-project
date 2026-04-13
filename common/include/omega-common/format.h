#include "utils.h"
#include <ctime>
#include <tuple>
#include <array>
#include <memory>

#ifndef OMEGA_COMMON_FORMAT_H
#define OMEGA_COMMON_FORMAT_H

namespace OmegaCommon {

    enum class LogLevel : int {
        Debug = 0,
        Info = 1,
        Warn = 2,
        Error = 3,
    };

    class OMEGACOMMON_EXPORT LogSink {
    public:
        virtual ~LogSink() = default;
        virtual void log(LogLevel level, StrRef message) = 0;
    };

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
        T object;
        void insertFormattedObject(std::ostream &os) override {
            Pr::format(os,object);
        };

        template<std::enable_if_t<_has_format_provider<T>::value,int> = 0>
                explicit ObjectFormatProvider(T object):object(std::move(object)){

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
            if(object.data() != nullptr && object.size() > 0){
                os.write(object.data(), static_cast<std::streamsize>(object.size()));
            }
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
    OMEGACOMMON_EXPORT const char * logLevelName(LogLevel level);
    OMEGACOMMON_EXPORT void setLogSink(std::shared_ptr<LogSink> sink);
    OMEGACOMMON_EXPORT std::shared_ptr<LogSink> getLogSink();
    OMEGACOMMON_EXPORT void setLogMinimumLevel(LogLevel level);
    OMEGACOMMON_EXPORT LogLevel getLogMinimumLevel();
    OMEGACOMMON_EXPORT bool shouldLog(LogLevel level);
    OMEGACOMMON_EXPORT void logMessage(LogLevel level, StrRef message);

    template<typename T>
     ObjectFormatProvider<std::decay_t<T>> * buildFormatProvider(T && object){
        using Ty = std::decay_t<T>;
        return new ObjectFormatProvider<Ty>(std::forward<T>(object));
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
    void Log(LogLevel level, const char *fmt,_Args && ...args){
        if(!shouldLog(level)){
            return;
        }
        auto message = fmtString(fmt,std::forward<_Args>(args)...);
        logMessage(level,message);
    }

    template<class ..._Args>
    void LogDebug(const char *fmt,_Args && ...args){
        Log(LogLevel::Debug,fmt,std::forward<_Args>(args)...);
    }

    template<class ..._Args>
    void LogInfo(const char *fmt,_Args && ...args){
        Log(LogLevel::Info,fmt,std::forward<_Args>(args)...);
    }

    template<class ..._Args>
    void LogWarn(const char *fmt,_Args && ...args){
        Log(LogLevel::Warn,fmt,std::forward<_Args>(args)...);
    }

    template<class ..._Args>
    void LogError(const char *fmt,_Args && ...args){
        Log(LogLevel::Error,fmt,std::forward<_Args>(args)...);
    }

    template<class ..._Args>
    void LogV(const char *fmt,_Args && ...args){
        LogInfo(fmt,std::forward<_Args>(args)...);
    }
    
};

#endif
