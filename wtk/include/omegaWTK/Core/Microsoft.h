#ifndef OMEGAWTK_CORE_MICROSOFT_H
#define OMEGAWTK_CORE_MICROSOFT_H   
#include <wrl.h>
#pragma comment(lib,"runtimeobject.lib")
namespace OmegaWTK::Core {
    template <class T> void SafeRelease(T **ppT)
    {
        if (*ppT)
        {
            (*ppT)->Release();
            *ppT = NULL;
        }
    }            
    /// A ComPtr that releases its object on its destruction. (Similar to the std::unique_ptr)
    template<class T>
    class UniqueComPtr {
    public:
        Microsoft::WRL::ComPtr<T> comPtr;
        T * get() { return comPtr.Get();};
        T * operator->(){
            return comPtr.Get();
        };
        T ** operator&(){
            return comPtr.GetAddressOf();
        };
        UniqueComPtr() = default;
        // UniqueComPtr(Microsoft::WRL::ComPtr<T> _com):comPtr(_com){};
        UniqueComPtr(T *ptr):comPtr(ptr){};
        ~UniqueComPtr(){
            auto ptr = comPtr.Detach();
            Core::SafeRelease(&ptr);
        };
    };
};
#endif // OMEGAWTK_CORE_MICROSOFT_H
