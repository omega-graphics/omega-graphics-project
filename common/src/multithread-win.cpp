#include "omega-common/multithread.h"

namespace OmegaCommon {

    Semaphore::Semaphore(int initalValue){
        sem = CreateSemaphoreA(NULL,initalValue,initalValue,NULL);
    };
    void Semaphore::get(){
        BOOL block = TRUE;
        while(block) {
            DWORD signal = WaitForSingleObject(sem,0L);
            switch (signal) {
                case WAIT_OBJECT_0 : {
                    block = FALSE;
                    break;
                }
            }
        } 
    };
    void Semaphore::release(){
        assert(!ReleaseSemaphore(sem,1,nullptr) && "Failed to Release Semaphore");
    }
    Semaphore::~Semaphore(){
        CloseHandle(sem);
    };

    Pipe::Pipe(){
        SECURITY_ATTRIBUTES securityAttributes;
        securityAttributes.bInheritHandle = TRUE;
        securityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
        securityAttributes.lpSecurityDescriptor = NULL;
        CreatePipe(&file_a,&file_b,&securityAttributes,0);

        SetHandleInformation(file_a,HANDLE_FLAG_INHERIT,0);
    }

    void Pipe::setCurrentProcessAsA() {
        ///
    }

    void Pipe::setCurrentProcessAsB() {
        ///
    }

    size_t Pipe::readA(char *buffer, size_t n_read) {
        DWORD _n_read;
       BOOL success = ReadFile(file_a,(LPVOID)buffer,DWORD(n_read),&_n_read,NULL);
       if(!success){
           return 0;
       }
        return (size_t)_n_read;
    }

    size_t Pipe::readB(char *buffer, size_t n_read) {
        DWORD _n_read;
        BOOL success = ReadFile(file_b,(LPVOID)buffer,DWORD(n_read),&_n_read,NULL);
        if(!success){
            return 0;
        }
        return (size_t)_n_read;
    }

    void Pipe::writeA(char *buffer, size_t n_write) {
        WriteFile(file_a,(LPCVOID)buffer,DWORD(n_write),NULL,NULL);
    }

    void Pipe::writeB(char *buffer, size_t n_write) {
        WriteFile(file_b,(LPCVOID)buffer,DWORD(n_write),NULL,NULL);
    }

    Pipe::~Pipe(){
        CloseHandle(file_a);
        CloseHandle(file_b);
        CloseHandle(h);
    }


    ChildProcess ChildProcess::Open(const OmegaCommon::String &cmd, const OmegaCommon::Vector<const char *> &args) {
        STARTUPINFO startupinfo;
        ZeroMemory(&startupinfo,sizeof(STARTUPINFO));
        startupinfo.cb = sizeof(STARTUPINFO);

        ChildProcess process{};
        process.use_pipe = false;
        ZeroMemory(&process.processInformation,sizeof(process.processInformation));

        CreateProcessA(NULL,(LPSTR)cmd.data(),NULL,NULL,FALSE,0,NULL,NULL,&startupinfo,&process.processInformation);

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
        process.use_pipe = true;
        ZeroMemory(&process.processInformation,sizeof(process.processInformation));
        startupinfo.hStdOutput = process.pipe.file_b;
        startupinfo.hStdError = process.pipe.file_b;
//    file_b    startupinfo.dwFlags |= STARTF_USESTDHANDLES;

        BOOL f = CreateProcessA(NULL,(LPSTR)(OmegaCommon::String(cmd) + args).c_str(),&securityAttributes,NULL,FALSE,0,NULL,NULL,&startupinfo,&process.processInformation);


        if(f == FALSE){
            std::cerr << "ERROR:" << cmd << " is not a valid command!" << std::endl;
            exit(1);
        }

        return process;
    }

    int ChildProcess::wait() {
//        std::cout << "Waiting" << std::endl;
        WaitForSingleObject(processInformation.hProcess,INFINITE);
        std::cout << "Process has terminated" << std::endl;
        DWORD exit_code;

        GetExitCodeProcess(processInformation.hProcess,&exit_code);
//        std::cout << "Exit Code:" << exit_code << std::endl;

        CloseHandle(processInformation.hThread);
        CloseHandle(processInformation.hProcess);
//        std::cout << "Close Process" << exit_code << std::endl;
        off = true;
        return int(exit_code);
    }



    ChildProcess::~ChildProcess() {
        if(!off){
            auto rc = wait();

        }
    }
    
}