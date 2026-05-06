#include "omegaWTK/Composition/Brush.h"

#include <algorithm>
#include <cmath>

namespace OmegaWTK::Composition {

namespace {

inline float clamp01(float v){
    return std::max(0.f, std::min(1.f, v));
}

/// Wrap a hue (degrees) into [0, 360).
inline float wrapHue(float h){
    h = std::fmod(h, 360.f);
    if(h < 0.f) h += 360.f;
    return h;
}

/// HSV/HSL share the segment selector. Given a hue in degrees and
/// (chroma, intermediate) values, produce the un-offset (r, g, b).
inline void hueToRGBSegment(float h, float c, float x, float & r, float & g, float & b){
    const float hp = wrapHue(h) / 60.f;
    if      (hp < 1.f) { r = c; g = x; b = 0.f; }
    else if (hp < 2.f) { r = x; g = c; b = 0.f; }
    else if (hp < 3.f) { r = 0.f; g = c; b = x; }
    else if (hp < 4.f) { r = 0.f; g = x; b = c; }
    else if (hp < 5.f) { r = x; g = 0.f; b = c; }
    else               { r = c; g = 0.f; b = x; }
}

}  // namespace

Color Color::create8Bit(std::uint8_t r,std::uint8_t g,std::uint8_t b,std::uint8_t a)
{
    Color color;
    color.r = float(r)/0xFF;
    color.g = float(g)/0xFF;
    color.b = float(b)/0xFF;
    color.a = float(a)/0xFF;
    return color;
};

Color Color::create16Bit(std::uint16_t r,std::uint16_t g,std::uint16_t b,std::uint16_t a)
{
    Color color;
    color.r = float(r)/0xFFFF;
    color.g = float(g)/0xFFFF;
    color.b = float(b)/0xFFFF;
    color.a = float(a)/0xFFFF;
    return color;
};

Color Color::create8Bit(std::uint32_t hex_color,std::uint8_t alpha){
    uint8_t mask = 0xFF;
    Color c {};
    /// BB
    c.b = float(hex_color & mask)/float(mask);
    hex_color = hex_color >> 8;
    /// GG
    c.g = float(hex_color & mask)/float(mask);
    hex_color = hex_color >> 8;
    /// RR
    c.r = float(hex_color & mask)/float(mask);

    c.a = float(alpha)/float(mask);
    return c;
};

Color Color::create16Bit(std::uint64_t hex_color,std::uint16_t alpha){
    uint16_t mask = 0xFFFF;
    Color c {};
    /// BB
    c.b = float(hex_color & mask)/float(mask);
    hex_color = hex_color >> 16;
    /// GG
    c.g = float(hex_color & mask)/float(mask);
    hex_color = hex_color >> 16;
    /// RR
    c.r = float(hex_color & mask)/float(mask);

    c.a = float(alpha)/float(mask);
    return c;
};

Color Color::fromHSV(float h,float s,float v,float a){
    s = clamp01(s);
    v = clamp01(v);
    const float c = v * s;
    const float hp = wrapHue(h) / 60.f;
    const float x = c * (1.f - std::fabs(std::fmod(hp, 2.f) - 1.f));
    const float m = v - c;
    float r = 0.f, g = 0.f, b = 0.f;
    hueToRGBSegment(h, c, x, r, g, b);
    Color out;
    out.r = r + m;
    out.g = g + m;
    out.b = b + m;
    out.a = clamp01(a);
    return out;
}

Color Color::fromHSL(float h,float s,float l,float a){
    s = clamp01(s);
    l = clamp01(l);
    const float c = (1.f - std::fabs((2.f * l) - 1.f)) * s;
    const float hp = wrapHue(h) / 60.f;
    const float x = c * (1.f - std::fabs(std::fmod(hp, 2.f) - 1.f));
    const float m = l - (c * 0.5f);
    float r = 0.f, g = 0.f, b = 0.f;
    hueToRGBSegment(h, c, x, r, g, b);
    Color out;
    out.r = r + m;
    out.g = g + m;
    out.b = b + m;
    out.a = clamp01(a);
    return out;
}

Color Color::lerp(const Color & a,const Color & b,float t){
    t = clamp01(t);
    Color out;
    out.r = a.r + (b.r - a.r) * t;
    out.g = a.g + (b.g - a.g) * t;
    out.b = a.b + (b.b - a.b) * t;
    out.a = a.a + (b.a - a.a) * t;
    return out;
}

Color Color::withAlpha(float newAlpha) const {
    Color out = *this;
    out.a = clamp01(newAlpha);
    return out;
}

Color Color::lighter(float amount) const {
    // Lerp toward opaque white, preserving this color's alpha.
    Color white = Color::White.withAlpha(a);
    return Color::lerp(*this, white, amount);
}

Color Color::darker(float amount) const {
    // Lerp toward opaque black, preserving this color's alpha.
    Color black = Color::Black.withAlpha(a);
    return Color::lerp(*this, black, amount);
}

bool Color::compare(const Color &other){
    return (r == other.r) && (g == other.g) &&(b == other.b) && (a == other.a);
};

OMEGAWTK_EXPORT const Color Color::Transparent = Color::create8Bit(Color::Eight::Black8,0x00);
OMEGAWTK_EXPORT const Color Color::Black  = Color::create8Bit(Color::Eight::Black8);
OMEGAWTK_EXPORT const Color Color::White  = Color::create8Bit(Color::Eight::White8);
OMEGAWTK_EXPORT const Color Color::Red    = Color::create8Bit(Color::Eight::Red8);
OMEGAWTK_EXPORT const Color Color::Green  = Color::create8Bit(Color::Eight::Green8);
OMEGAWTK_EXPORT const Color Color::Blue   = Color::create8Bit(Color::Eight::Blue8);
OMEGAWTK_EXPORT const Color Color::Yellow = Color::create8Bit(Color::Eight::Yellow8);
OMEGAWTK_EXPORT const Color Color::Orange = Color::create8Bit(Color::Eight::Orange8);
OMEGAWTK_EXPORT const Color Color::Purple = Color::create8Bit(Color::Eight::Purple8);

Gradient::GradientStop Gradient::Stop(float pos, Color color){
    return {pos,color};
};

Gradient Gradient::Linear(std::initializer_list<GradientStop> stops,float angle){
    Gradient grad;
    grad.stops = stops;
    grad.arg = angle;
    grad.type = Type::Linear;
    return grad;
};

Gradient Gradient::Radial(std::initializer_list<GradientStop> stops,float radius){
    Gradient grad;
    grad.stops = stops;
    grad.arg = radius;
    grad.type = Type::Radial;
    return grad;
};

Brush::Brush(const Color & color):type(Type::Color),color(color){

};

Brush::Brush(const Gradient & gradient):type(Type::Gradient),gradient(gradient){

};

Brush::~Brush(){
    switch(type){
        case Type::Color:
            color.~Color();
            break;
        case Type::Gradient:
            gradient.~Gradient();
            break;
        case Type::None:
            break;
    }
}

Core::SharedPtr<Brush> ColorBrush(const Color & color){
    return std::make_shared<Brush>(color);
};

Core::SharedPtr<Brush> GradientBrush(const Gradient & gradient){
    return std::make_shared<Brush>(gradient);
};

}
