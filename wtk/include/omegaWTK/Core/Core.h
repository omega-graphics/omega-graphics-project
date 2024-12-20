#include <omega-common/common.h>

#include <OmegaGTE.h>

#include <cassert>
#include <iomanip>
#include <memory>


#include "OmegaWTKExport.h"
#include "Unicode.h"

#ifdef TARGET_WIN32
#include <wrl.h>
#pragma comment(lib,"runtimeobject.lib")
#endif

#ifndef OMEGAWTK_CORE_CORE_H
#define OMEGAWTK_CORE_CORE_H

/// @brief OmegaWTK
/// A cross platform user interface api.
namespace OmegaWTK {
    

    extern OmegaGTE::GTE gte;

    #ifdef INTERFACE 
    #undef INTERFACE
    #endif
    #define INTERFACE class

    #define INTERFACE_METHOD virtual

    #define ABSTRACT = 0;

    #define FALLTHROUGH = default;

    #define DEFAULT {};
//
//    #define DELEGATE INTERFACE
//
//    #define DELEGATE_METHOD INTERFACE_METHOD

    // inline const char *time_stamp(){
    //     time_t tt;
    //     tt = std::time(nullptr);
    //     tm *ti = std::localtime(&tt);

    //     return std::asctime(ti);

        
    // };
        

    #define OMEGAWTK_DEBUG(msg) ::std::cout << "[OmegaWTKDebug " << " ] - " << msg << ::std::endl;
    
    typedef enum : int {
        CodeOk,
        CodeFailed
    } StatusCode;



    namespace Core {
         typedef unsigned char Option;
    };

#define STATIC_OPT static constexpr Core::Option
#define OPT_PARAM Core::Option

    namespace Core {

        template<class _Ty>
        using UniquePtr = std::unique_ptr<_Ty>;
    
        template<class Ty>
        class OMEGAWTK_EXPORT UniquePtrRef {
            UniquePtr<Ty> & ptr;
        public:
            bool hasExpired(){
                return ptr == nullptr;
            };
            void resetRef(UniquePtr<Ty> & _new_ptr){
                ptr = _new_ptr;
            };
            explicit UniquePtrRef(UniquePtr<Ty> & _ptr):ptr(_ptr){};
            ~UniquePtrRef() = default;
        };
    
        template<class _Ty>
        using SharedPtr = std::shared_ptr<_Ty>;
    
        template<class _Ty>
        using WeakPtr = std::weak_ptr<_Ty>;

        template<class _Ty>
        using Optional = std::optional<_Ty>;
    
        typedef std::istream IStream;
        typedef std::ostream OStream;


        typedef OmegaGTE::GRect Rect;

        typedef OmegaGTE::GPoint2D Position;

        typedef OmegaGTE::GRoundedRect RoundedRect;

        struct OMEGAWTK_EXPORT Ellipse : private OmegaGTE::GEllipsoid {
            float x;
            float y;
            float rad_x;
            float rad_y;

            Ellipse():OmegaGTE::GEllipsoid(){}

            Ellipse(float x,float y,float rad_x,float rad_y):
            GEllipsoid({x,y,0,rad_x,rad_y,0}),
            x(OmegaGTE::GEllipsoid::x),
            y(OmegaGTE::GEllipsoid::y),
            rad_x(OmegaGTE::GEllipsoid::rad_x),
            rad_y(OmegaGTE::GEllipsoid::rad_y){
            };
        };




        #ifdef TARGET_WIN32
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
        #endif

        // class OMEGAWTK_EXPORT RegularExpression {
        //     pcre2_code *code;
        // public:
        //     RegularExpression(String pattern,bool multiLine = true);
            
        //     struct Match {
        //         pcre2_match_data *mdata;
        //     public:
        //         String main;
        //         String getSubMatchByNum(unsigned n);
        //         ~Match();
        //     };
        //     Match match(String subject);
        //     ~RegularExpression();
        // };

        // typedef RegularExpression Regex;
    };

    void loadAssetFile(OmegaCommon::FS::Path path);

     template<class Ty>
    class StatusWithObj {
        StatusCode code;
        std::shared_ptr<Ty> data;
        char * message;

    public:
        operator bool(){
            return code == CodeOk;
        };
        StatusCode getCode(){ return code;};
        const char * getError(){ return message;};
        Core::SharedPtr<Ty> getValue(){
           return data;
        };
        StatusWithObj(const Ty & obj):message(nullptr){
            data = std::make_shared<Ty>(std::move(obj));
        };

        StatusWithObj(Ty && obj):message(nullptr){
             data = std::make_shared<Ty>(obj);
        };

        StatusWithObj(const char * message):data(nullptr){
            auto len = strlen(message);
            this->message = new char[len];
            std::move((char *)message,(char *)message + len,this->message);
            code = CodeFailed;
        };
        ~StatusWithObj(){
            // if(data != nullptr){
            //     data->~_Ty();
            //     delete data;
            // }
        };
    };

}


//template<class _Ty>
//OMEGAWTK_EXPORT using WeakHandle = OmegaWTK::Core::WeakPtr<_Ty>;

#endif
