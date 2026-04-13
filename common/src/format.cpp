#include "omega-common/format.h"
#include <cassert>
#include <cctype>
#include <mutex>

namespace OmegaCommon {

    namespace {

        class StdIOLogSink : public LogSink {
            std::mutex writeMutex;
        public:
            void log(LogLevel level, StrRef message) override {
                auto & out = (level == LogLevel::Warn || level == LogLevel::Error) ? std::cerr : std::cout;
                std::lock_guard<std::mutex> lock(writeMutex);
                out << "[" << logLevelName(level) << "] ";
                if(message.data() != nullptr && message.size() > 0){
                    out.write(message.data(), static_cast<std::streamsize>(message.size()));
                }
                out << std::endl;
            }
        };

        struct LogState {
            std::mutex mutex;
            LogLevel minimumLevel = LogLevel::Info;
            std::shared_ptr<LogSink> sink = std::make_shared<StdIOLogSink>();
        };

        LogState & globalLogState() {
            static LogState state;
            return state;
        }

    } // namespace

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

    const char * logLevelName(LogLevel level) {
        switch(level){
            case LogLevel::Debug:
                return "DEBUG";
            case LogLevel::Info:
                return "INFO";
            case LogLevel::Warn:
                return "WARN";
            case LogLevel::Error:
                return "ERROR";
        }
        return "INFO";
    }

    void setLogSink(std::shared_ptr<LogSink> sink) {
        if(!sink){
            sink = std::make_shared<StdIOLogSink>();
        }
        auto & state = globalLogState();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.sink = std::move(sink);
    }

    std::shared_ptr<LogSink> getLogSink() {
        auto & state = globalLogState();
        std::lock_guard<std::mutex> lock(state.mutex);
        return state.sink;
    }

    void setLogMinimumLevel(LogLevel level) {
        auto & state = globalLogState();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.minimumLevel = level;
    }

    LogLevel getLogMinimumLevel() {
        auto & state = globalLogState();
        std::lock_guard<std::mutex> lock(state.mutex);
        return state.minimumLevel;
    }

    bool shouldLog(LogLevel level) {
        auto & state = globalLogState();
        std::lock_guard<std::mutex> lock(state.mutex);
        return static_cast<int>(level) >= static_cast<int>(state.minimumLevel);
    }

    void logMessage(LogLevel level, StrRef message) {
        std::shared_ptr<LogSink> sink;
        {
            auto & state = globalLogState();
            std::lock_guard<std::mutex> lock(state.mutex);
            if(static_cast<int>(level) < static_cast<int>(state.minimumLevel)){
                return;
            }
            sink = state.sink;
        }

        if(sink){
            sink->log(level,message);
        }
    }

} // namespace OmegaCommon
