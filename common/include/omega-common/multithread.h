

#ifndef OMEGA_COMMON_MULTITHREAD_H
#define OMEGA_COMMON_MULTITHREAD_H

#include "utils.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <future>
#include <vector>
#include <utility>
#include <memory>

#include <cassert>

namespace OmegaCommon {
    typedef std::thread Thread;
    typedef std::mutex Mutex;

    template<class T>
    class Async {
        std::shared_ptr<bool> hasValue;
        std::shared_ptr<Mutex> mutex;
        std::shared_ptr<std::condition_variable> condition;
        std::shared_ptr<T> _val;

        template<class Ty>
        friend class Promise;

    public:
        explicit Async(std::shared_ptr<bool> hasValue,
                       std::shared_ptr<Mutex> mutex,
                       std::shared_ptr<std::condition_variable> condition,
                       std::shared_ptr<T> _val):
        hasValue(hasValue),
        mutex(mutex),
        condition(condition),
        _val(_val){

        }
        bool ready(){
            std::lock_guard<Mutex> lk(*mutex.get());
            return *hasValue;
        }
        T & get(){
            std::unique_lock<Mutex> lk(*mutex.get());
            condition->wait(lk,[this](){
                return *hasValue;
            });
            return *_val;
        }
        ~Async() = default;
    };

    template<class T>
    class Promise {

        std::shared_ptr<Mutex>  mutex;
        std::shared_ptr<bool> hasValue;
        std::shared_ptr<std::condition_variable> condition;
        std::shared_ptr<T> val;
    public:
        Promise():mutex(std::make_shared<Mutex>()),
                  hasValue(std::make_shared<bool>(false)),
                  condition(std::make_shared<std::condition_variable>()),
                  val(std::make_shared<T>()){

        };
        Promise(const Promise &) = delete;
        Promise(Promise && prom):
                mutex(prom.mutex),
                hasValue(prom.hasValue),
                condition(prom.condition),
                val(prom.val){
            
        }
        Async<T> async(){
            return Async<T>{hasValue,mutex,condition,val};
        };
        void set(const T & v){
           bool wake = false;
           {
               std::lock_guard<Mutex> lk(*mutex.get());
               if(!(*hasValue)){
                *val = v;
                *hasValue = true;
                wake = true;
               }
           }
           if(wake){
               condition->notify_all();
           }
        }
        void set(T && v){
            bool wake = false;
            {
                std::lock_guard<Mutex> lk(*mutex.get());
                if(!(*hasValue)){
                    *val = std::move(v);
                    *hasValue = true;
                    wake = true;
                }
            }
            if(wake){
                condition->notify_all();
            }
        }
        ~Promise() = default;
    };

    class OMEGACOMMON_EXPORT Semaphore {
        struct Impl;
        std::unique_ptr<Impl> impl;
    public:
        explicit Semaphore(int initialValue);
        Semaphore(const Semaphore &) = delete;
        Semaphore & operator=(const Semaphore &) = delete;
        Semaphore(Semaphore &&) noexcept;
        Semaphore & operator=(Semaphore &&) noexcept;
        void release();
        void get();
        ~Semaphore();
    };

    /// @brief A unidirectional transport bridge.
    /// @paragraph
    /// The pipe in this implementation is represented a one-way bridge between Point A and Point B.
    class OMEGACOMMON_EXPORT Pipe {
        struct Impl;
        bool sideA;
        std::unique_ptr<Impl> impl;
        friend class ChildProcess;
        Pipe();
        Pipe(const Pipe &) = delete;
        Pipe & operator=(const Pipe &) = delete;
        Pipe(Pipe &&) noexcept;
        Pipe & operator=(Pipe &&) noexcept;
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
        struct Impl;
        std::unique_ptr<Impl> impl;
    public:
        ChildProcess();
        ChildProcess(const ChildProcess &) = delete;
        ChildProcess & operator=(const ChildProcess &) = delete;
        ChildProcess(ChildProcess &&) noexcept;
        ChildProcess & operator=(ChildProcess &&) noexcept;
        static ChildProcess OpenWithStdoutPipe(const OmegaCommon::String & cmd,const char * args);
        static ChildProcess Open(const OmegaCommon::String & cmd,const OmegaCommon::Vector<const char *> & args);
        int wait();
        ~ChildProcess();
    };
    /**
     * @brief Assignable thread farm designed for completing multithreaded tasks.
     * 
     */
    class OMEGACOMMON_EXPORT WorkerFarm {
        std::vector<Thread> farm;
        std::mutex mutex;
    public:
        WorkerFarm() = default;
        WorkerFarm(const WorkerFarm &) = delete;
        WorkerFarm & operator=(const WorkerFarm &) = delete;
        WorkerFarm(WorkerFarm &&) = delete;
        WorkerFarm & operator=(WorkerFarm &&) = delete;

        template<class FnT,typename ...Args>
        void scheduleJob(FnT && func,Args && ...args){
            std::lock_guard<std::mutex> guard(mutex);
            farm.emplace_back(std::forward<FnT>(func),std::forward<Args>(args)...);
        }

        ~WorkerFarm(){
            std::vector<Thread> pending;
            {
                std::lock_guard<std::mutex> guard(mutex);
                pending.swap(farm);
            }
            for(auto &thread : pending){
                if(thread.joinable()){
                    thread.join();
                }
            }
        }
    };

};

#endif
