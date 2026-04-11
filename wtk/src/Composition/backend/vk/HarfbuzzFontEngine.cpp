#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Core/GTEHandle.h"

#include "omega-common/fs.h"

#include <pango/pangocairo.h>
#include <fontconfig/fontconfig.h>
#include <cairo/cairo.h>
#include <gdk/gdk.h>

#include <cstring>

namespace OmegaWTK::Composition {

    static double getScreenScaleFactor(){
        GdkDisplay *display = gdk_display_get_default();
        if(display != nullptr){
            GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
            if(monitor == nullptr){
                monitor = gdk_display_get_monitor(display,0);
            }
            if(monitor != nullptr){
                return (double)gdk_monitor_get_scale_factor(monitor);
            }
        }
        return 1.0;
    }

    static PangoAlignment toPangoAlignment(TextLayoutDescriptor::Alignment alignment){
        switch(alignment){
            case TextLayoutDescriptor::LeftUpper:
            case TextLayoutDescriptor::LeftCenter:
            case TextLayoutDescriptor::LeftLower:
                return PANGO_ALIGN_LEFT;
            case TextLayoutDescriptor::MiddleUpper:
            case TextLayoutDescriptor::MiddleCenter:
            case TextLayoutDescriptor::MiddleLower:
                return PANGO_ALIGN_CENTER;
            case TextLayoutDescriptor::RightUpper:
            case TextLayoutDescriptor::RightCenter:
            case TextLayoutDescriptor::RightLower:
                return PANGO_ALIGN_RIGHT;
            default:
                return PANGO_ALIGN_LEFT;
        }
    }

    static PangoWrapMode toPangoWrapMode(TextLayoutDescriptor::Wrapping wrapping){
        switch(wrapping){
            case TextLayoutDescriptor::WrapByWord:
                return PANGO_WRAP_WORD;
            case TextLayoutDescriptor::WrapByCharacter:
                return PANGO_WRAP_CHAR;
            case TextLayoutDescriptor::None:
            default:
                return PANGO_WRAP_WORD;
        }
    }

    /// Returns the vertical alignment category from a TextLayoutDescriptor::Alignment.
    /// 0 = upper, 1 = center, 2 = lower
    static int verticalAlignmentCategory(TextLayoutDescriptor::Alignment alignment){
        switch(alignment){
            case TextLayoutDescriptor::LeftUpper:
            case TextLayoutDescriptor::MiddleUpper:
            case TextLayoutDescriptor::RightUpper:
                return 0;
            case TextLayoutDescriptor::LeftCenter:
            case TextLayoutDescriptor::MiddleCenter:
            case TextLayoutDescriptor::RightCenter:
                return 1;
            case TextLayoutDescriptor::LeftLower:
            case TextLayoutDescriptor::MiddleLower:
            case TextLayoutDescriptor::RightLower:
                return 2;
            default:
                return 0;
        }
    }

    class HarfBuzzFont : public Font {
        PangoFontDescription *fontDesc;
    public:
        explicit HarfBuzzFont(FontDescriptor &desc, PangoFontDescription *fontDesc)
            :Font(desc), fontDesc(fontDesc){
        }

        void * getNativeFont() override {
            return fontDesc;
        }

        ~HarfBuzzFont()  {
            if(fontDesc != nullptr){
                pango_font_description_free(fontDesc);
            }
        }
    };

    class HarfBuzzGlyphRun : public GlyphRun {
    public:
        OmegaWTK::UniString str;
        Core::SharedPtr<HarfBuzzFont> font;

        HarfBuzzGlyphRun(const OmegaWTK::UniString &str, Core::SharedPtr<Font> &font)
            :str(str), font(std::dynamic_pointer_cast<HarfBuzzFont>(font)){
        }

        Composition::Rect getBoundingRectOfGlyphAtIndex(size_t glyphIdx) override {
            (void)glyphIdx;
            return {};
        }
    };

    class HarfBuzzTextRect : public TextRect {
        TextLayoutDescriptor layoutDesc;
        unsigned char *pixelData = nullptr;
        size_t pixelWidth = 0;
        size_t pixelHeight = 0;
        size_t bytesPerRow = 0;
    public:
        explicit HarfBuzzTextRect(Composition::Rect rect, const TextLayoutDescriptor &layoutDesc)
            :TextRect(rect), layoutDesc(layoutDesc){
        }

        void drawRun(Core::SharedPtr<GlyphRun> &glyphRun, const Composition::Color &color) override {
            auto gr = std::dynamic_pointer_cast<HarfBuzzGlyphRun>(glyphRun);
            if(gr == nullptr){
                return;
            }

            double scaleFactor = getScreenScaleFactor();
            pixelWidth = (size_t)(rect.w * scaleFactor);
            pixelHeight = (size_t)(rect.h * scaleFactor);
            bytesPerRow = pixelWidth * 4;
            size_t byteCount = bytesPerRow * pixelHeight;

            delete[] pixelData;
            pixelData = new unsigned char[byteCount];
            std::memset(pixelData, 0, byteCount);

            cairo_surface_t *surface = cairo_image_surface_create_for_data(
                pixelData,
                CAIRO_FORMAT_ARGB32,
                (int)pixelWidth,
                (int)pixelHeight,
                (int)bytesPerRow);

            cairo_t *cr = cairo_create(surface);
            cairo_set_antialias(cr, CAIRO_ANTIALIAS_SUBPIXEL);

            PangoLayout *layout = pango_cairo_create_layout(cr);

            // Convert UTF-16 string to UTF-8 for Pango
            const UnicodeChar *u16Buf = gr->str.getBuffer();
            int32_t u16Len = gr->str.length();
            if(u16Buf != nullptr && u16Len > 0){
                GError *err = nullptr;
                gchar *utf8 = g_utf16_to_utf8(
                    (const gunichar2 *)u16Buf,
                    (glong)u16Len,
                    nullptr, nullptr, &err);
                if(utf8 != nullptr){
                    pango_layout_set_text(layout, utf8, -1);
                    g_free(utf8);
                }
                if(err != nullptr){
                    g_error_free(err);
                }
            }

            // Apply font
            if(gr->font != nullptr){
                auto *srcDesc = (PangoFontDescription *)gr->font->getNativeFont();
                if(srcDesc != nullptr){
                    PangoFontDescription *scaled = pango_font_description_copy(srcDesc);
                    int origSize = pango_font_description_get_size(scaled);
                    pango_font_description_set_size(scaled, (int)(origSize * scaleFactor));
                    pango_layout_set_font_description(layout, scaled);
                    pango_font_description_free(scaled);
                }
            }

            // Layout parameters
            if(layoutDesc.wrapping != TextLayoutDescriptor::None){
                pango_layout_set_wrap(layout, toPangoWrapMode(layoutDesc.wrapping));
                pango_layout_set_width(layout, (int)(pixelWidth * PANGO_SCALE));
            } else {
                pango_layout_set_width(layout, -1);
            }

            pango_layout_set_alignment(layout, toPangoAlignment(layoutDesc.alignment));

            if(layoutDesc.lineLimit > 0){
                pango_layout_set_height(layout, -(int)layoutDesc.lineLimit);
            }

            // Compute vertical offset for center/lower alignment
            double yOffset = 0.0;
            int vAlign = verticalAlignmentCategory(layoutDesc.alignment);
            if(vAlign != 0){
                int layoutW, layoutH;
                pango_layout_get_pixel_size(layout, &layoutW, &layoutH);
                if(vAlign == 1){
                    yOffset = ((double)pixelHeight - layoutH) / 2.0;
                } else {
                    yOffset = (double)pixelHeight - layoutH;
                }
                if(yOffset < 0.0) yOffset = 0.0;
            }

            // Draw
            cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
            cairo_move_to(cr, 0.0, yOffset);
            pango_cairo_show_layout(cr, layout);
            cairo_surface_flush(surface);

            g_object_unref(layout);
            cairo_destroy(cr);
            cairo_surface_destroy(surface);
        }

        BitmapRes toBitmap() override {
            BitmapRes res {nullptr, nullptr};
            if(pixelData == nullptr || pixelWidth == 0 || pixelHeight == 0){
                return res;
            }

            OmegaGTE::TextureDescriptor desc {};
            desc.usage = OmegaGTE::GETexture::ToGPU;
            desc.storage_opts = OmegaGTE::Shared;
            desc.pixelFormat = OmegaGTE::TexturePixelFormat::BGRA8Unorm;
            desc.type = OmegaGTE::GETexture::Texture2D;
            desc.width = (unsigned)pixelWidth;
            desc.height = (unsigned)pixelHeight;

            auto texture = gte.graphicsEngine->makeTexture(desc);
            texture->copyBytes(pixelData, bytesPerRow);

            res.s = texture;
            return res;
        }

        void *getNative() override {
            return nullptr;
        }

        ~HarfBuzzTextRect() override {
            delete[] pixelData;
        }
    };

    FontEngine *FontEngine::instance = nullptr;

    class HarfBuzzFontEngine : public FontEngine {
    public:
        HarfBuzzFontEngine() = default;
        ~HarfBuzzFontEngine() override = default;

        Core::SharedPtr<Font> CreateFont(FontDescriptor &desc) override {
            PangoFontDescription *fontDesc = pango_font_description_new();
            pango_font_description_set_family(fontDesc, desc.family.c_str());
            pango_font_description_set_size(fontDesc, (gint)desc.size * PANGO_SCALE);

            switch(desc.style){
                case FontDescriptor::Bold:
                    pango_font_description_set_weight(fontDesc, PANGO_WEIGHT_BOLD);
                    break;
                case FontDescriptor::Italic:
                    pango_font_description_set_style(fontDesc, PANGO_STYLE_ITALIC);
                    break;
                case FontDescriptor::BoldAndItalic:
                    pango_font_description_set_weight(fontDesc, PANGO_WEIGHT_BOLD);
                    pango_font_description_set_style(fontDesc, PANGO_STYLE_ITALIC);
                    break;
                case FontDescriptor::Regular:
                default:
                    break;
            }

            return Core::SharedPtr<Font>(new HarfBuzzFont(desc, fontDesc));
        }

        Core::SharedPtr<Font> CreateFontFromFile(OmegaCommon::FS::Path path, FontDescriptor &desc) override {
            FcConfigAppFontAddFile(FcConfigGetCurrent(),
                (const FcChar8 *)path.str().c_str());

            return CreateFont(desc);
        }
    };

    FontEngine *FontEngine::inst(){
        return instance;
    }

    void FontEngine::Create(){
        if(instance == nullptr){
            instance = new HarfBuzzFontEngine();
        }
    }

    void FontEngine::Destroy(){
        delete instance;
        instance = nullptr;
    }

    Core::SharedPtr<GlyphRun>
    GlyphRun::fromUStringAndFont(const OmegaWTK::UniString &str, Core::SharedPtr<Font> &font) {
        return Core::SharedPtr<GlyphRun>(new HarfBuzzGlyphRun(str, font));
    }

    Core::SharedPtr<TextRect> TextRect::Create(Composition::Rect rect, const TextLayoutDescriptor &layoutDesc){
        return Core::SharedPtr<TextRect>(new HarfBuzzTextRect(rect, layoutDesc));
    }

}
