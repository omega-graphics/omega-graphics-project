#include "omega-common/multithread.h"

#include <Windows.h>

namespace OmegaCommon {

    struct Semaphore::Impl {
        HANDLE sem = nullptr;

        explicit Impl(int initialValue) {
            sem = CreateSemaphoreA(NULL,initialValue,initialValue,NULL);
        }

        ~Impl() {
            if(sem != nullptr){
                CloseHandle(sem);
            }
        }
    };

    Semaphore::Semaphore(int initalValue): impl(new Impl(initalValue)) {
    }

    Semaphore::Semaphore(Semaphore &&) noexcept = default;

    Semaphore & Semaphore::operator=(Semaphore &&) noexcept = default;

    void Semaphore::get() {
        BOOL block = TRUE;
        while(block) {
            DWORD signal = WaitForSingleObject(impl->sem,0L);
            switch (signal) {
                case WAIT_OBJECT_0 : {
                    block = FALSE;
                    break;
                }
            }
        }
    }

    void Semaphore::release() {
        assert(!ReleaseSemaphore(impl->sem,1,nullptr) && "Failed to Release Semaphore");
    }

    Semaphore::~Semaphore() = default;

    struct Pipe::Impl {
        HANDLE h = nullptr;
        HANDLE file_a = nullptr;
        HANDLE file_b = nullptr;

        ~Impl() {
            if(file_a != nullptr){
                CloseHandle(file_a);
            }
            if(file_b != nullptr){
                CloseHandle(file_b);
            }
            if(h != nullptr){
                CloseHandle(h);
            }
        }
    };

    Pipe::Pipe(): sideA(true), impl(new Impl()) {
        SECURITY_ATTRIBUTES securityAttributes;
        securityAttributes.bInheritHandle = TRUE;
        securityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        securityAttributes.lpSecurityDescriptor = NULL;
        CreatePipe(&impl->file_a,&impl->file_b,&securityAttributes,0);

        SetHandleInformation(impl->file_a,HANDLE_FLAG_INHERIT,0);
    }

    Pipe::Pipe(Pipe &&) noexcept = default;

    Pipe & Pipe::operator=(Pipe &&) noexcept = default;

    void Pipe::setCurrentProcessAsA() {
        ///
    }

    void Pipe::setCurrentProcessAsB() {
        ///
    }

    size_t Pipe::readA(char *buffer, size_t n_read) {
        DWORD _n_read;
        BOOL success = ReadFile(impl->file_a,(LPVOID)buffer,DWORD(n_read),&_n_read,NULL);
        if(!success){
            return 0;
        }
        return (size_t)_n_read;
    }

    void Pipe::writeA(char *buffer, size_t n_write) {
        WriteFile(impl->file_a,(LPCVOID)buffer,DWORD(n_write),NULL,NULL);
    }

    size_t Pipe::readB(char *buffer, size_t n_read) {
        DWORD _n_read;
        BOOL success = ReadFile(impl->file_b,(LPVOID)buffer,DWORD(n_read),&_n_read,NULL);
        if(!success){
            return 0;
        }
        return (size_t)_n_read;
    }

    void Pipe::writeB(char *buffer, size_t n_write) {
        WriteFile(impl->file_b,(LPCVOID)buffer,DWORD(n_write),NULL,NULL);
    }

    Pipe::~Pipe() = default;

    struct ChildProcess::Impl {
        bool off = false;
        PROCESS_INFORMATION processInformation{};
        std::unique_ptr<Pipe> pipe;
        bool use_pipe = false;
    };

    ChildProcess::ChildProcess(): impl(new Impl()) {
    }

    ChildProcess::ChildProcess(ChildProcess &&) noexcept = default;

    ChildProcess & ChildProcess::operator=(ChildProcess &&) noexcept = default;

    ChildProcess ChildProcess::Open(const OmegaCommon::String &cmd, const OmegaCommon::Vector<const char *> &args) {
        (void)args;

        STARTUPINFO startupinfo;
        ZeroMemory(&startupinfo,sizeof(STARTUPINFO));
        startupinfo.cb = sizeof(STARTUPINFO);

        ChildProcess process{};
        process.impl->use_pipe = false;
        ZeroMemory(&process.impl->processInformation,sizeof(process.impl->processInformation));

        CreateProcessA(NULL,(LPSTR)cmd.data(),NULL,NULL,FALSE,0,NULL,NULL,&startupinfo,&process.impl->processInformation);

        return process;
    }

    ChildProcess ChildProcess::OpenWithStdoutPipe(const OmegaCommon::String &cmd, const char *args) {
        STARTUPINFO startupinfo;
        ZeroMemory(&startupinfo,sizeof(STARTUPINFO));
        startupinfo.cb = sizeof(STARTUPINFO);

        SECURITY_ATTRIBUTES securityAttributes;
        ZeroMemory(&securityAttributes,sizeof(SECURITY_ATTRIBUTES));

        securityAttributes.lpSecurityDescriptor = NULL;
        securityAttributes.bInheritHandle = TRUE;
        securityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);


        ChildProcess process{};
        process.impl->use_pipe = true;
        process.impl->pipe.reset(new Pipe());
        ZeroMemory(&process.impl->processInformation,sizeof(process.impl->processInformation));
        startupinfo.hStdOutput = process.impl->pipe->impl->file_b;
        startupinfo.hStdError = process.impl->pipe->impl->file_b;

        BOOL f = CreateProcessA(NULL,(LPSTR)(OmegaCommon::String(cmd) + args).c_str(),&securityAttributes,NULL,FALSE,0,NULL,NULL,&startupinfo,&process.impl->processInformation);


        if(f == FALSE){
            std::cerr << "ERROR:" << cmd << " is not a valid command!" << std::endl;
            exit(1);
        }

        return process;
    }

    int ChildProcess::wait() {
        if(!impl){
            return -1;
        }

        WaitForSingleObject(impl->processInformation.hProcess,INFINITE);
        std::cout << "Process has terminated" << std::endl;
        DWORD exit_code;

        GetExitCodeProcess(impl->processInformation.hProcess,&exit_code);

        CloseHandle(impl->processInformation.hThread);
        CloseHandle(impl->processInformation.hProcess);
        impl->off = true;
        return int(exit_code);
    }

    ChildProcess::~ChildProcess() {
        if(impl != nullptr && !impl->off){
            wait();
        }
    }

}
