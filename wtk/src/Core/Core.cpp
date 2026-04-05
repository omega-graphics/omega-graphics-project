#include "omegaWTK/Core/Core.h"
#include <cstring>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <iostream>
#include <regex>


namespace OmegaWTK {
    OmegaGTE::GTE gte;
}
namespace OmegaWTK {


Core::Rect Rect(float x,float y,float w,float h){
    return {{x,y},w,h};
};

Core::RoundedRect RoundedRect(float x,float y,float w,float h,float radius_x,float radius_y) {
    return {{x,y},w,h,radius_x,radius_y};
}




}
