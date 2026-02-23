#import "NativePrivate/macos/CocoaUtils.h"

#include <algorithm>
#include <cmath>

namespace OmegaWTK::Native::Cocoa {

namespace {

static inline CGFloat safeScale(){
    CGFloat scale = [NSScreen mainScreen].backingScaleFactor;
    if(scale <= 0.f || !std::isfinite(static_cast<double>(scale))){
        scale = 2.f;
    }
    return std::max(scale,static_cast<CGFloat>(2.f));
}

static inline CGFloat clampPointDimension(float value){
    constexpr CGFloat kMaxDrawableDimension = 16384.f;
    const CGFloat scale = safeScale();
    const CGFloat maxPoints = kMaxDrawableDimension / scale;
    if(!std::isfinite(static_cast<double>(value)) || value <= 0.f){
        return 1.f;
    }
    return std::min(std::max(static_cast<CGFloat>(value),static_cast<CGFloat>(1.f)),maxPoints);
}

static inline CGFloat clampCoordinate(float value){
    if(!std::isfinite(static_cast<double>(value))){
        return 0.f;
    }
    return static_cast<CGFloat>(value);
}

}

void ns_string_to_common_string(NSString *str,OmegaCommon::String & res){
    res.assign(str.UTF8String);
};

NSString * common_string_to_ns_string(const OmegaCommon::String & str){
    NSString *nsString = [[NSString alloc] initWithBytes:str.data()
                                                  length:str.size()
                                                encoding:NSUTF8StringEncoding];
    if(nsString == nil){
        nsString = [[NSString alloc] initWithUTF8String:""];
    }
    return [nsString autorelease];
};

NSRect core_rect_to_cg_rect(const Core::Rect & rect){
    auto x = clampCoordinate(rect.pos.x);
    auto y = clampCoordinate(rect.pos.y);
    auto w = clampPointDimension(rect.w);
    auto h = clampPointDimension(rect.h);
    std::cout << "X:" << x  << "Y:" << y << "W:" << w << "H:" << h;
    return NSMakeRect(x,y,w,h);
};



CGPoint core_pos_to_cg_point(const Core::Position & pos){
    return CGPointMake(pos.x,pos.y);
};

};
