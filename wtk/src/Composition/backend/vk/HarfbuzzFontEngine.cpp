#include "omegaWTK/Composition/FontEngine.h"

#include "omega-common/fs.h"

#include <hb.h>
#include <hb-icu.h>

namespace OmegaWTK::Composition {

    typedef OmegaCommon::Vector<hb_font_t *>  HBFontVector;

    FontEngine *FontEngine::instance = nullptr;

    class HarfBuzzFont : public Font {
        hb_blob_t *file_blob = nullptr;
        hb_face_t *face = nullptr;
    public:
        explicit HarfBuzzFont(FontDescriptor &desc):Font(desc){
        }

        void * getNativeFont() override {
            return face;
        }
    };

    class HarfBuzzGlyphRun : public GlyphRun {
    public:
        explicit HarfBuzzGlyphRun(const OmegaWTK::UniString &str, Core::SharedPtr<Font> &font){
            (void)str;
            (void)font;
        }

        Core::Rect getBoundingRectOfGlyphAtIndex(size_t glyphIdx) override {
            (void)glyphIdx;
            return Core::Rect {{0.f,0.f},0.f,0.f};
        }
    };

    class HarfBuzzTextRect : public TextRect {
    public:
        explicit HarfBuzzTextRect(Core::Rect rect):TextRect(rect){
        }

        BitmapRes toBitmap() override {
            return {nullptr,nullptr};
        }

        void *getNative() override {
            return nullptr;
        }

        void drawRun(Core::SharedPtr<GlyphRun> &glyphRun,const Composition::Color &color) override {
            (void)glyphRun;
            (void)color;
        }
    };

    FontEngine::FontEngine() = default;

    FontEngine *FontEngine::inst(){
        return instance;
    }

    void FontEngine::Create(){
        if(instance == nullptr){
            instance = new FontEngine();
        }
    }

    void FontEngine::Destroy(){
        delete instance;
        instance = nullptr;
    }

    FontEngine::~FontEngine() = default;

    void loadFontFromFile(OmegaCommon::StrRef file,hb_blob_t **blob_out,hb_face_t **face_out){
        *blob_out = hb_blob_create_from_file(file.data());
        *face_out = hb_face_create(*blob_out,0);
    }

    void getBuiltinFonts(HBFontVector & out){
        /// Check for TrueType fonts
        auto trueTypePath = "/usr/share/fonts/truetype"_FS_PATH;

        auto ttFontsExists = OmegaCommon::FS::exists(trueTypePath);

        if(ttFontsExists){
            auto dir_it = OmegaCommon::FS::DirectoryIterator(trueTypePath);
            while(!dir_it.end()){
                auto file = *dir_it;
                if(file.ext() == "ttf"){
                    hb_blob_t *blob;
                    hb_face_t *face;
                    loadFontFromFile(file.str(), &blob, &face);
                    out.push_back(hb_font_create(face));
                }
                ++dir_it;
            }
        }

    };

    void freeFonts(HBFontVector & fonts){
        for(auto & f : fonts)
            hb_font_destroy(f);
    }

    Core::SharedPtr<GlyphRun>
    GlyphRun::fromUStringAndFont(const OmegaWTK::UniString &str, Core::SharedPtr<Font> &font) {
        return Core::SharedPtr<GlyphRun>(new HarfBuzzGlyphRun(str,font));
    }

    Core::SharedPtr<TextRect> TextRect::Create(Core::Rect rect,const TextLayoutDescriptor & layoutDesc){
        (void)layoutDesc;
        return Core::SharedPtr<TextRect>(new HarfBuzzTextRect(rect));
    }

    Core::SharedPtr<Font> FontEngine::CreateFont(FontDescriptor & desc){
        HBFontVector fonts;
        getBuiltinFonts(fonts);
        freeFonts(fonts);
        return Core::SharedPtr<Font>(new HarfBuzzFont(desc));
    };

     Core::SharedPtr<Font> FontEngine::CreateFontFromFile(OmegaCommon::FS::Path path,FontDescriptor & desc){
        (void)path;
        return Core::SharedPtr<Font>(new HarfBuzzFont(desc));
     };

}
