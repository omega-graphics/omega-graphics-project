#include "omegaWTK/Composition/FontEngine.h"
#include "NativePrivate/macos/CocoaUtils.h"

#include "omegaWTK/Core/Unicode.h"

#import <CoreText/CoreText.h>
#include <memory>
#include <cstring>
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

namespace OmegaWTK::Composition {

static CGFloat currentScreenScale(){
    CGFloat scale = [NSScreen mainScreen].backingScaleFactor;
    if(scale <= 0.f){
        scale = 1.f;
    }
    return scale;
}

static NSTextAlignment toNSTextAlignment(TextLayoutDescriptor::Alignment alignment){
    switch(alignment){
        case TextLayoutDescriptor::LeftUpper:
        case TextLayoutDescriptor::LeftCenter:
        case TextLayoutDescriptor::LeftLower:
            return NSTextAlignmentLeft;
        case TextLayoutDescriptor::MiddleUpper:
        case TextLayoutDescriptor::MiddleCenter:
        case TextLayoutDescriptor::MiddleLower:
            return NSTextAlignmentCenter;
        case TextLayoutDescriptor::RightUpper:
        case TextLayoutDescriptor::RightCenter:
        case TextLayoutDescriptor::RightLower:
            return NSTextAlignmentRight;
        default:
            return NSTextAlignmentLeft;
    }
}

static NSLineBreakMode toNSLineBreakMode(TextLayoutDescriptor::Wrapping wrapping){
    switch(wrapping){
        case TextLayoutDescriptor::WrapByWord:
            return NSLineBreakByWordWrapping;
        case TextLayoutDescriptor::WrapByCharacter:
            return NSLineBreakByCharWrapping;
        case TextLayoutDescriptor::None:
        default:
            return NSLineBreakByClipping;
    }
}


 FontEngine * FontEngine::instance;

 FontEngine::FontEngine(){
    
 };

 FontEngine::~FontEngine() = default;

FontEngine *FontEngine::inst() {
    return instance;
}

 void FontEngine::Create(){
         instance = new FontEngine();
     };

     void FontEngine::Destroy(){
         delete instance;
     };



 class CoreTextFont : public Font {
     CTFontRef native;
     friend class CTTextRect;
 public:
     CoreTextFont(FontDescriptor & desc,CTFontRef ref):Font(desc),native(ref){};
     void * getNativeFont(){
         return (void *)native;
     };
 };

class CTGlyphRun : public GlyphRun {
public:
    NSAttributedString *str;
    Core::SharedPtr<CoreTextFont> font;

    Core::Rect getBoundingRectOfGlyphAtIndex(size_t glyphIdx) override {
        return {};
    }

};

Core::SharedPtr<GlyphRun>
GlyphRun::fromUStringAndFont(const OmegaWTK::UniString &str, Core::SharedPtr<Font> &font) {
    auto run = new CTGlyphRun();
    run->font = std::dynamic_pointer_cast<CoreTextFont>(font);
    auto text = [NSString stringWithCharacters:(const unichar *)str.getBuffer() length:str.length()];
    auto nativeFont = (run->font != nullptr) ? (CTFontRef)run->font->getNativeFont() : nullptr;
    if(nativeFont != nullptr){
        run->str = [[NSAttributedString alloc] initWithString:text
                                                    attributes:@{NSFontAttributeName:(__bridge id)nativeFont}];
    }
    else {
        run->str = [[NSAttributedString alloc] initWithString:text];
    }
    return (Core::SharedPtr<GlyphRun>)run;
}

 class CTTextRect : public TextRect {
     CTFramesetterRef framesetterRef;
     CTFrameRef frame;
     NSAttributedString *strData;
     TextLayoutDescriptor layoutDesc;
     void _updateStrInternal(){
        
     };
 public:
     CTTextRect(Core::Rect & rect,const TextLayoutDescriptor &layoutDesc):
     TextRect(rect),
     framesetterRef(nullptr),
     frame(nullptr),
     strData(nil),
     layoutDesc(layoutDesc){
         NSLog(@"CTTextRect Create With W: %f H: %f",rect.w,rect.h);
     };
     void drawRun(Core::SharedPtr<GlyphRun> &glyphRun, const Composition::Color &color) override {
          auto gr = std::dynamic_pointer_cast<CTGlyphRun>(glyphRun);
          if(gr == nullptr || gr->str == nil){
              return;
          }
          if(frame != nullptr){
              CFRelease(frame);
              frame = nullptr;
          }
          if(framesetterRef != nullptr){
              CFRelease(framesetterRef);
              framesetterRef = nullptr;
          }

          auto attributed = [[NSMutableAttributedString alloc] initWithAttributedString:gr->str];
          auto range = NSMakeRange(0,attributed.length);
          auto textColor = [NSColor colorWithSRGBRed:color.r green:color.g blue:color.b alpha:color.a];
          auto paragraphStyle = [[NSMutableParagraphStyle alloc] init];
          [paragraphStyle setAlignment:toNSTextAlignment(layoutDesc.alignment)];
          [paragraphStyle setLineBreakMode:toNSLineBreakMode(layoutDesc.wrapping)];

          if(range.length > 0){
              [attributed addAttribute:NSForegroundColorAttributeName value:textColor range:range];
              [attributed addAttribute:NSParagraphStyleAttributeName value:paragraphStyle range:range];
          }

          auto nativeFont = (gr->font != nullptr) ? (CTFontRef)gr->font->getNativeFont() : nullptr;
          if(range.length > 0 && nativeFont != nullptr){
              [attributed addAttribute:NSFontAttributeName value:(__bridge id)nativeFont range:range];
          }

          strData = attributed;

          CGFloat scaleFactor = currentScreenScale();
          CGPathRef textPath = CGPathCreateWithRect(CGRectMake(0.f,0.f,rect.w * scaleFactor,rect.h * scaleFactor),NULL);
          framesetterRef = CTFramesetterCreateWithAttributedString((__bridge CFAttributedStringRef)strData);
          CFRange frameRange = CFRangeMake(0,strData.length);
          if(layoutDesc.lineLimit > 0){
              CTFrameRef preliminaryFrame = CTFramesetterCreateFrame(framesetterRef,frameRange,textPath,NULL);
              if(preliminaryFrame != nullptr){
                  CFArrayRef lines = CTFrameGetLines(preliminaryFrame);
                  CFIndex lineCount = lines != nullptr ? CFArrayGetCount(lines) : 0;
                  if(lineCount > static_cast<CFIndex>(layoutDesc.lineLimit)){
                      CTLineRef lastAllowedLine = (CTLineRef)CFArrayGetValueAtIndex(
                              lines,
                              static_cast<CFIndex>(layoutDesc.lineLimit) - 1);
                      if(lastAllowedLine != nullptr){
                          CFRange visibleRange = CTLineGetStringRange(lastAllowedLine);
                          CFIndex endIndex = visibleRange.location + visibleRange.length;
                          if(endIndex > 0 && endIndex < frameRange.length){
                              frameRange.length = endIndex;
                          }
                      }
                  }
                  CFRelease(preliminaryFrame);
              }
          }
          frame = CTFramesetterCreateFrame(framesetterRef,frameRange,textPath,NULL);
          CGPathRelease(textPath);
     }
     void * getNative() override{
         return (void *)frame;
     };
     void getGlyphBoundingBoxes(Core::Rect **rects, unsigned * count){
         *count = 0;
         CoreTextFont *fontRef = nullptr;
         CFArrayRef lines = CTFrameGetLines(frame);
         for(unsigned idx = 0;idx < CFArrayGetCount(lines);idx++){
             CTLineRef line = (CTLineRef)CFArrayGetValueAtIndex(lines,idx);
             CFArrayRef runs =  CTLineGetGlyphRuns(line);
             for(unsigned j = 0;j < CFArrayGetCount(runs);j++){
                 CTRunRef run = (CTRunRef)CFArrayGetValueAtIndex(runs,j);
                 const CGGlyph *ptr = CTRunGetGlyphsPtr(run);
                 CFIndex count = CTRunGetGlyphCount(run);
                 CGRect *rect = new CGRect[count];
                 CTFontGetBoundingRectsForGlyphs(fontRef->native,kCTFontOrientationDefault,ptr,rect,count);
             };
         }
     };
     // void reload() {
        
     // };
     BitmapRes toBitmap() override{
        BitmapRes res;
         CGFloat scaleFactor = currentScreenScale();
         const size_t pixelWidth = size_t(rect.w * scaleFactor);
         const size_t pixelHeight = size_t(rect.h * scaleFactor);
         const size_t bytesPerRow = pixelWidth * 4;
         const size_t byteCount = bytesPerRow * pixelHeight;
         auto *data = new unsigned char[byteCount];
         std::memset(data,0,byteCount);
         
         CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
         CGContextRef context = CGBitmapContextCreateWithData(data,pixelWidth,pixelHeight,8,bytesPerRow,colorSpace,kCGImageAlphaPremultipliedLast,NULL,NULL);
         CGColorSpaceRelease(colorSpace);
         if(context == nullptr){
             delete [](unsigned char *) data;
             return res;
         }
         CGContextSetAllowsAntialiasing(context,true);
         CGContextSetShouldAntialias(context,true);
         CGContextSetInterpolationQuality(context,kCGInterpolationHigh);
         CGContextSetShouldSmoothFonts(context,true);
         CGContextSetAllowsFontSmoothing(context,true);
         CGContextSetShouldSubpixelPositionFonts(context,true);
         CGContextSetAllowsFontSubpixelPositioning(context,true);
         CGContextSetShouldSubpixelQuantizeFonts(context,true);
         CGContextSetAllowsFontSubpixelQuantization(context,true);
         CGContextSetTextMatrix(context,CGAffineTransformIdentity);
         if(frame != nullptr){
             CTFrameDraw(frame,context);
         }
         CGContextFlush(context);
         

         OmegaGTE::TextureDescriptor desc {};
         desc.usage = OmegaGTE::GETexture::ToGPU;
         desc.storage_opts = OmegaGTE::Shared;
         desc.pixelFormat = OmegaGTE::TexturePixelFormat::RGBA8Unorm;
         desc.type = OmegaGTE::GETexture::Texture2D;
         desc.width = (unsigned)pixelWidth;
         desc.height = (unsigned)pixelHeight;
         
         auto texture = gte.graphicsEngine->makeTexture(desc);
         NSLog(@"CGBitmapContextData: %p",data);
         texture->copyBytes(data,bytesPerRow);

        CGContextRelease(context);


         delete [](unsigned char *) data;
        res.s = texture;
         return res;
     };
     ~CTTextRect(){
         if(frame != nullptr){
             CFRelease(frame);
             frame = nullptr;
         }
         if(framesetterRef != nullptr){
             CFRelease(framesetterRef);
             framesetterRef = nullptr;
         }
     };
 };

 Core::SharedPtr<TextRect> TextRect::Create(Core::Rect rect,const TextLayoutDescriptor & layoutDesc){
     return Core::SharedPtr<TextRect>(new CTTextRect(rect,layoutDesc));
 };

 Core::SharedPtr<Font> FontEngine::CreateFont(FontDescriptor & desc){
     CTFontRef ref = CTFontCreateWithNameAndOptions((__bridge CFStringRef)[NSString stringWithUTF8String:desc.family.c_str()],CGFloat(desc.size),NULL,kCTFontOptionsPreferSystemFont);
     CTFontSymbolicTraits fontTraits;
    
     switch (desc.style) {
         case FontDescriptor::Bold:
             fontTraits = kCTFontTraitBold;
             break;
         case FontDescriptor::Italic:
             fontTraits = kCTFontTraitItalic;
             break;
         case FontDescriptor::BoldAndItalic:
             fontTraits = kCTFontTraitBold | kCTFontTraitItalic;
             break;
         case FontDescriptor::Regular :
             fontTraits = 0;
             break;
         default:
             break;
     }
    
     auto scaleFactor = currentScreenScale();
    
     CTFontRef _font_final = CTFontCreateCopyWithSymbolicTraits(ref,CGFloat(desc.size) * scaleFactor,NULL,kCTFontTraitBold | kCTFontTraitItalic, fontTraits);
     CFRelease(ref);
     return SharedHandle<Font>(new CoreTextFont(desc,_font_final));
 };

 Core::SharedPtr<Font> FontEngine::CreateFontFromFile(OmegaCommon::FS::Path path, FontDescriptor &desc){
     CTFontSymbolicTraits fontTraits;
    
     switch (desc.style) {
         case FontDescriptor::Bold:
             fontTraits = kCTFontTraitBold;
             break;
         case FontDescriptor::Italic:
             fontTraits = kCTFontTraitItalic;
             break;
         case FontDescriptor::BoldAndItalic:
             fontTraits = kCTFontTraitBold | kCTFontTraitItalic;
             break;
         case FontDescriptor::Regular :
             fontTraits = 0;
             break;
         default:
             break;
     }

     NSURL *url = [NSURL fileURLWithPath:Native::Cocoa::common_string_to_ns_string(path.absPath()) isDirectory:NO];
     CFArrayRef fontDescriptors = CTFontManagerCreateFontDescriptorsFromURL((__bridge CFURLRef)url);
     CTFontDescriptorRef idealFont;
     for(unsigned i = 0;i < CFArrayGetCount(fontDescriptors);i++){
         CTFontDescriptorRef fd = (CTFontDescriptorRef)CFArrayGetValueAtIndex(fontDescriptors,i);
         CFStringRef name = (CFStringRef)CTFontDescriptorCopyAttribute(fd,kCTFontNameAttribute);
         CFNumberRef symbolicTraits = (CFNumberRef)CTFontDescriptorCopyAttribute(fd,kCTFontSymbolicTrait);
         NSString *str = (__bridge id)name;
         int val;
         CFNumberGetValue(symbolicTraits,kCFNumberIntType,&val);
         if(desc.family == str.UTF8String && fontTraits == val){
             idealFont = fd;
         };
     };

     auto scaleFactor = currentScreenScale();

     CTFontRef f = CTFontCreateWithFontDescriptor(idealFont,CGFloat(desc.size) * scaleFactor,NULL);
     return std::make_shared<CoreTextFont>(desc,f);
 };
};
