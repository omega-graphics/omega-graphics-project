#include "omegaGTE/GTEBase.h"
#include "omegaGTE/GTEShader.h"

#ifdef TARGET_METAL
// Use strict Metal simd data type alignment
#include <simd/simd.h>
#endif


_NAMESPACE_BEGIN_

const long double PI = std::acos(-1);

size_t omegaSLStructSize(OmegaCommon::Vector<omegasl_data_type> data) noexcept{
    size_t s = 0;
    size_t biggestWord = 1;
    OmegaCommon::Vector<size_t> data_sizes;
    /// 1. Find all data type sizes and find biggest word.
    for(auto d : data){
        switch (d) {
            case OMEGASL_FLOAT :
            case OMEGASL_INT :
            {
                auto _s = sizeof(float);
                if(biggestWord < _s){
                    biggestWord = _s;
                }
                data_sizes.push_back(_s);
                break;
            }
            case OMEGASL_FLOAT2 :
            case OMEGASL_INT2 : {
#if defined(TARGET_METAL)
                auto _s = sizeof(simd_float2);
                if(biggestWord < _s){
                    biggestWord = _s;
                }
                data_sizes.push_back(_s);
#else
                auto _s = sizeof(float);
                if(biggestWord < _s){
                    biggestWord = _s;
                }
                data_sizes.push_back(_s);
                data_sizes.push_back(_s);

#endif
                break;
            }
            case OMEGASL_FLOAT3 :
            case OMEGASL_INT3 :
            {
#if defined(TARGET_METAL)
                auto _s = sizeof(simd_float3);
                if(biggestWord < _s){
                    biggestWord = _s;
                }
                data_sizes.push_back(_s);
#else
                auto _s = sizeof(float);
                if(biggestWord < _s){
                    biggestWord = _s;
                }
                data_sizes.push_back(_s);
                data_sizes.push_back(_s);
                data_sizes.push_back(_s);
#endif
                break;
            }
            case OMEGASL_FLOAT4 :
            case OMEGASL_INT4 :
            {
#if defined(TARGET_METAL)
                auto _s = sizeof(simd_float4);
                if(biggestWord < _s){
                    biggestWord = _s;
                }
                data_sizes.push_back(_s);
#else
                auto _s = sizeof(float);
                if(biggestWord < _s){
                    biggestWord = _s;
                }
                data_sizes.push_back(_s);
                data_sizes.push_back(_s);
                data_sizes.push_back(_s);
                data_sizes.push_back(_s);
#endif
                break;
            }
        }
    }

    /// 2. Add Sizes to Final Struct Size and align data to biggest word.
    bool afterBiggestWord = false;
    size_t s_after = 0;
    for(auto size : data_sizes){
        if(afterBiggestWord){
           s_after += size;
        }
        else {
            if (size == biggestWord) {
                size_t padding = 0;
                if(s > 0) {
                    padding = s % biggestWord;
                }
                s += padding + biggestWord;
                afterBiggestWord = true;
            }
            else {
                s += size;
            }
        }
    }

    if(s_after > 0){
        auto padding = s_after % biggestWord;
        s += padding + s_after;
    }

    return s;
}

_NAMESPACE_END_