#ifndef OMEGAVA_MICROSOFT_H
#define OMEGAVA_MICROSOFT_H

// TODO: lift these COM helpers into omega-common so OmegaWTK and OmegaVA
// share one implementation (mirrors the duplication this file currently has
// with wtk/include/omegaWTK/Core/Microsoft.h).

#include <wrl.h>
#pragma comment(lib,"runtimeobject.lib")

namespace OmegaVA::Core {

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
        UniqueComPtr(T *ptr):comPtr(ptr){};
        ~UniqueComPtr(){
            auto ptr = comPtr.Detach();
            Core::SafeRelease(&ptr);
        };
    };

};

#endif // OMEGAVA_MICROSOFT_H
