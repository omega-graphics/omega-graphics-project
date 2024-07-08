
#include "omega-common/multithread.h"

#ifndef __APPLE__
#include <fcntl.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif



namespace OmegaCommon {



#if defined(__APPLE__)

    Semaphore::Semaphore(int initialValue):sem(dispatch_semaphore_create(initialValue)) {

    }

    void Semaphore::get() {
        dispatch_semaphore_wait(sem,DISPATCH_TIME_FOREVER);
    }

    void Semaphore::release() {
        dispatch_semaphore_signal(sem);
    }

    Semaphore::~Semaphore() {
        dispatch_release(sem);
    }
#else

    Semaphore::Semaphore(int initialValue){
        sem_init(&sem,0,0);
    };

    void Semaphore::get(){
        sem_wait(&sem);
    }

    void Semaphore::release(){
        sem_post(&sem);
    }

    Semaphore::~Semaphore(){
        sem_close(&sem);
    }

#endif

    ChildProcess ChildProcess::Open(const OmegaCommon::String &cmd, const OmegaCommon::Vector<const char *> &args) {
        ChildProcess process;
        pid_t pid;
        /// Child Process and Parent Process Split
        if((pid = fork()) == 0){
            /// CHILD PROCESS!
            auto rc = execv(cmd.data(),(char *const *)args.data());
            exit(rc);
        }
        else {
            process.pid = pid;
            process.use_pipe = false;
        }
        return process;
    }

    ChildProcess ChildProcess::OpenWithStdoutPipe(const OmegaCommon::String &cmd, const char *args) {
        ChildProcess process;
        process.use_pipe = true;
        auto f = popen((OmegaCommon::String(cmd) + args).c_str(),"r");
        process.p_file = f;
        return process;
    }

    int ChildProcess::wait() {
        int rc;

        /// PARENT PROCESS!
        if(use_pipe){
            if(p_file == nullptr){
                return -1;
            }
            char *data;
            auto len = fseeko(p_file,1,SEEK_END);
            if(len > 0){
                fseeko(p_file,0,SEEK_SET);
                data = new char[len];
                fread(data,1,len,p_file);
                std::cout << data << std::endl;
                delete data;
            }

            rc = pclose(p_file);
            p_file = nullptr;
        }
        else {
            waitpid(pid,&rc,0);
            pid = -1;
        }
        return rc;
    }

    ChildProcess::~ChildProcess() {
        if(pid != -1)
            wait();
    }

    Pipe::Pipe():sideA(true) {
        pipe(pipe_fd);
    }

    void Pipe::setCurrentProcessAsA() {
        close(pipe_fd[1]);
        sideA = true;
    }

    void Pipe::setCurrentProcessAsB() {
        close(pipe_fd[0]);
        sideA = false;
    }

    size_t Pipe::readA(char *buffer, size_t n_read) {
        assert(!sideA);
        return read(pipe_fd[0],buffer,n_read);
    }

    size_t Pipe::readB(char *buffer, size_t n_read) {
        assert(sideA);
        return read(pipe_fd[1],buffer,n_read);
    }

    void Pipe::writeA(char *buffer, size_t n_write) {
        assert(!sideA);
        write(pipe_fd[1],buffer,n_write);
    }

    void Pipe::writeB(char *buffer, size_t n_write) {
        assert(sideA);
        write(pipe_fd[0],buffer,n_write);
    }

    Pipe::~Pipe(){

    }



}