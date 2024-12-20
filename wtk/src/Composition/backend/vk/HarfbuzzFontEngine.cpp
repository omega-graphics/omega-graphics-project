#include "omegaWTK/Composition/FontEngine.h"

#include "omega-common/fs.h"

#include <hb.h>
#include <hb-icu.h>

namespace OmegaWTK::Composition {

    typedef OmegaCommon::Vector<hb_font_t *>  HBFontVector;

    FontEngine::FontEngine(){

    }

    void FontEngine::Create(){
        instance = new FontEngine();
    }

    void FontEngine::Destroy(){
        delete instance;
    }

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

    class HarfBuzzFont : public Font {
        hb_blob_t *file_blob;
        hb_face_t *face;
    public:
        void * getNativeFont() override {
            return face;
        }
    };

    Core::SharedPtr<Font> FontEngine::CreateFont(FontDescriptor & desc){
        HBFontVector fonts;
        getBuiltinFonts(fonts);
        // Assume the standard fetch language for fonts is English ()
        return make<HarfBuzzFont>();
    };

     Core::SharedPtr<Font> FontEngine::CreateFontFromFile(OmegaCommon::FS::Path path,FontDescriptor & desc){
        return make<HarfBuzzFont>();
     };

}