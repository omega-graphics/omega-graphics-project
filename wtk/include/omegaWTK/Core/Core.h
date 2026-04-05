#include <omega-common/common.h>

#include <OmegaGTE.h>

#include <cassert>
#include <iomanip>
#include <memory>
#include <string>


#include "OmegaWTKExport.h"
#include "Unicode.h"


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
    };

    void loadAssetFile(OmegaCommon::FS::Path path);

     template<class Ty>
    class StatusWithObj {
        StatusCode code;
        std::shared_ptr<Ty> data;
        std::string message;

    public:
        operator bool() const {
            return code == CodeOk;
        };
        StatusCode getCode() const { return code;};
        const char * getError() const { return message.c_str();};
        Core::SharedPtr<Ty> getValue() const {
           return data;
        };
        StatusWithObj(const Ty & obj):code(CodeOk),data(std::make_shared<Ty>(obj)){
        };

        StatusWithObj(Ty && obj):code(CodeOk),data(std::make_shared<Ty>(std::move(obj))){
        };

        StatusWithObj(const char * message):code(CodeFailed),data(nullptr),message(message ? message : ""){
        };
        ~StatusWithObj() = default;
    };

}


//template<class _Ty>
//OMEGAWTK_EXPORT using WeakHandle = OmegaWTK::Core::WeakPtr<_Ty>;

#endif
