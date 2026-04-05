#include "omega-common/multithread.h"

#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <fcntl.h>
#include <semaphore.h>
#include <sys/stat.h>
#endif

namespace OmegaCommon {

#if defined(__APPLE__)

    struct Semaphore::Impl {
        dispatch_semaphore_t sem;

        explicit Impl(int initialValue): sem(dispatch_semaphore_create(initialValue)) {
        }

        ~Impl() {
            dispatch_release(sem);
        }
    };

#else

    struct Semaphore::Impl {
        sem_t sem;

        explicit Impl(int initialValue) {
            sem_init(&sem,0,initialValue);
        }

        ~Impl() {
            sem_destroy(&sem);
        }
    };

#endif

    Semaphore::Semaphore(int initialValue): impl(new Impl(initialValue)) {
    }

    Semaphore::Semaphore(Semaphore &&) noexcept = default;

    Semaphore & Semaphore::operator=(Semaphore &&) noexcept = default;

    void Semaphore::get() {
#if defined(__APPLE__)
        dispatch_semaphore_wait(impl->sem,DISPATCH_TIME_FOREVER);
#else
        sem_wait(&impl->sem);
#endif
    }

    void Semaphore::release() {
#if defined(__APPLE__)
        dispatch_semaphore_signal(impl->sem);
#else
        sem_post(&impl->sem);
#endif
    }

    Semaphore::~Semaphore() = default;

    struct Pipe::Impl {
        int pipe_fd[2] = {-1,-1};

        Impl() {
            pipe(pipe_fd);
        }

        ~Impl() {
            if(pipe_fd[0] != -1){
                close(pipe_fd[0]);
            }
            if(pipe_fd[1] != -1){
                close(pipe_fd[1]);
            }
        }
    };

    Pipe::Pipe(): sideA(true), impl(new Impl()) {
    }

    Pipe::Pipe(Pipe &&) noexcept = default;

    Pipe & Pipe::operator=(Pipe &&) noexcept = default;

    void Pipe::setCurrentProcessAsA() {
        if(impl->pipe_fd[1] != -1){
            close(impl->pipe_fd[1]);
            impl->pipe_fd[1] = -1;
        }
        sideA = true;
    }

    void Pipe::setCurrentProcessAsB() {
        if(impl->pipe_fd[0] != -1){
            close(impl->pipe_fd[0]);
            impl->pipe_fd[0] = -1;
        }
        sideA = false;
    }

    size_t Pipe::readA(char *buffer, size_t n_read) {
        assert(!sideA);
        return read(impl->pipe_fd[0],buffer,n_read);
    }

    void Pipe::writeA(char *buffer, size_t n_write) {
        assert(!sideA);
        write(impl->pipe_fd[1],buffer,n_write);
    }

    size_t Pipe::readB(char *buffer, size_t n_read) {
        assert(sideA);
        return read(impl->pipe_fd[1],buffer,n_read);
    }

    void Pipe::writeB(char *buffer, size_t n_write) {
        assert(sideA);
        write(impl->pipe_fd[0],buffer,n_write);
    }

    Pipe::~Pipe() = default;

    struct ChildProcess::Impl {
        FILE *p_file = nullptr;
        bool use_pipe = false;
        pid_t pid = -1;
    };

    ChildProcess::ChildProcess(): impl(new Impl()) {
    }

    ChildProcess::ChildProcess(ChildProcess &&) noexcept = default;

    ChildProcess & ChildProcess::operator=(ChildProcess &&) noexcept = default;

    ChildProcess ChildProcess::Open(const OmegaCommon::String &cmd, const OmegaCommon::Vector<const char *> &args) {
        ChildProcess process;
        pid_t pid;
        if((pid = fork()) == 0){
            auto rc = execv(cmd.data(),(char *const *)args.data());
            exit(rc);
        }
        else {
            process.impl->pid = pid;
            process.impl->use_pipe = false;
        }
        return process;
    }

    ChildProcess ChildProcess::OpenWithStdoutPipe(const OmegaCommon::String &cmd, const char *args) {
        ChildProcess process;
        process.impl->use_pipe = true;
        process.impl->p_file = popen((OmegaCommon::String(cmd) + args).c_str(),"r");
        return process;
    }

    int ChildProcess::wait() {
        if(!impl){
            return -1;
        }

        int rc = -1;
        if(impl->use_pipe){
            if(impl->p_file == nullptr){
                return -1;
            }
            auto len = fseeko(impl->p_file,1,SEEK_END);
            if(len > 0){
                fseeko(impl->p_file,0,SEEK_SET);
                auto *data = new char[len + 1]{};
                fread(data,1,len,impl->p_file);
                std::cout << data << std::endl;
                delete [] data;
            }

            rc = pclose(impl->p_file);
            impl->p_file = nullptr;
        }
        else if(impl->pid != -1) {
            waitpid(impl->pid,&rc,0);
            impl->pid = -1;
        }
        return rc;
    }

    ChildProcess::~ChildProcess() {
        if(impl != nullptr && ((impl->use_pipe && impl->p_file != nullptr) || impl->pid != -1)){
            wait();
        }
    }

}
