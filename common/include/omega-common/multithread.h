

#ifndef OMEGA_COMMON_MULTITHREAD_H
#define OMEGA_COMMON_MULTITHREAD_H

#include "utils.h"
#ifdef _WIN32
#include <Windows.h>
#elif defined(__APPLE__)

#include <dispatch/dispatch.h>

#else
#include <semaphore.h>
#endif
#include <mutex>
#include <thread>
#include <future>

#include <cassert>

namespace OmegaCommon {
    typedef std::thread Thread;
    typedef std::mutex Mutex;

    template<class T>
    class Async {
        bool * hasValue;
        Mutex * mutex;
        T * _val;

        template<class Ty>
        friend class Promise;

    public:
        explicit Async(bool * hasValue, Mutex * mutex, T * _val):
        hasValue(hasValue),
        mutex(mutex),
        _val(_val){

        }
        bool ready(){
            std::lock_guard<Mutex> lk(*mutex);
            return *hasValue;
        }
        T & get(){
            while(!ready());
            return *_val;
        }
        ~Async() = default;
    };

    template<class T>
    class Promise {

        std::shared_ptr<Mutex>  mutex;
        std::shared_ptr<bool> hasValue;
        std::shared_ptr<T> val;
    public:
        Promise():mutex(new Mutex()),hasValue(std::make_shared<bool>(false)),val((T *)new T()){

        };
        Promise(const Promise &) = delete;
        Promise(Promise && prom):
                mutex(prom.mutex),
                hasValue(prom.hasValue),
                val(prom.val){
            
        }
        Async<T> async(){
            return Async<T>{hasValue.get(),mutex.get(),val.get()};
        };
        void set(const T & v){
           std::lock_guard<Mutex> lk(*mutex);
           if(!hasValue){
                new(val.get()) T(v);
                *hasValue = true;
           }
        }
        void set(T && v){
           std::lock_guard<Mutex> lk(*mutex);
           if(!hasValue){
                new(val.get()) T(v);
                *hasValue = true;
           }
        }
        ~Promise() = default;
    };

    class OMEGACOMMON_EXPORT Semaphore {
        #ifdef _WIN32
        HANDLE sem;
        #elif defined(__APPLE__)
        dispatch_semaphore_t sem;
        #else
        sem_t sem;
        #endif
    public:
        explicit Semaphore(int initialValue);
        void release();
        void get();
        ~Semaphore();
    };

    /// @brief A unidirectional transport bridge.
    /// @paragraph
    /// The pipe in this implementation is represented a one-way bridge between Point A and Point B.
    class OMEGACOMMON_EXPORT Pipe {
        bool sideA;
        friend class ChildProcess;
#ifdef _WIN32
        HANDLE h;
        HANDLE file_a,file_b;
#else
        int pipe_fd[2];
#endif
        Pipe();
        /// @brief Sets the Current Process As Point A
        void setCurrentProcessAsA();
        /// @brief Sets the Current Process As Point B
        void setCurrentProcessAsB();

        size_t readA(char *buffer,size_t n_read);
        void writeA(char *buffer,size_t n_write);

        size_t readB(char *buffer,size_t n_read);
        void writeB(char *buffer,size_t n_write);

        ~Pipe();
    };

    /// @brief A Subprocess of the current process.
    class OMEGACOMMON_EXPORT ChildProcess {
#ifdef _WIN32
        bool off = false;
        PROCESS_INFORMATION processInformation;
        STARTUPINFO startupinfo;
        Pipe pipe;
        bool use_pipe;
#else
        FILE *p_file = nullptr;
        bool use_pipe;
        pid_t pid;
#endif
    public:
        static ChildProcess OpenWithStdoutPipe(const OmegaCommon::String & cmd,const char * args);
        static ChildProcess Open(const OmegaCommon::String & cmd,const OmegaCommon::Vector<const char *> & args);
        int wait();
        ~ChildProcess();
    };

};

#endif


