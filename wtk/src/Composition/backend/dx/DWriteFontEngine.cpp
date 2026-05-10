#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "NativePrivate/win/WinUtils.h"
#include "omegaWTK/Core/Microsoft.h"
#include "omega-common/unicode.h"

#include <dwrite.h>
#include <dwrite_1.h>

#pragma comment(lib,"dwrite.lib")

#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dcommon.h>
#include <dxgi.h>
#include <dxgiformat.h>
#include <vector>

#pragma comment(lib,"dxguid.lib")
#pragma comment(lib,"d2d1.lib")

namespace OmegaWTK::Composition {

    class FontEnumerator : public IDWriteFontFileEnumerator {
        IDWriteFactory * dwrite_factory;
        unsigned refCount = 1;
        std::wstring font_file;
    public:
        explicit FontEnumerator(OmegaCommon::StrRefBase<wchar_t> font_file):font_file(font_file){

        };
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppvObject) override {
            if(iid == IID_IUnknown || iid == __uuidof(IDWriteFontFileEnumerator)){
                AddRef();
                *ppvObject = this;
                return S_OK;
            }
            else {
                *ppvObject = nullptr;
                return E_NOINTERFACE;
            }
        };
        ULONG STDMETHODCALLTYPE AddRef() override {
            return InterlockedIncrement(&refCount);
        };
        ULONG STDMETHODCALLTYPE Release() override{
            ULONG n = InterlockedDecrement(&refCount);
            if(n == 0){
                delete this;
            }
            return n;
        };
        explicit FontEnumerator(IDWriteFactory * dwrite_factory):dwrite_factory(dwrite_factory){

        }
        HRESULT MoveNext(BOOL *hasCurrentFile) noexcept override {
            return S_OK;
        }
        HRESULT GetCurrentFontFile(IDWriteFontFile **fontFile) noexcept override {
            HRESULT hr = dwrite_factory->CreateFontFileReference(font_file.c_str(),nullptr,fontFile);
            return hr;
        }
    };

/// @note The Collection Key is the font file path to be loaded.
    class FontLoader : public IDWriteFontCollectionLoader {
        unsigned refCount = 1;
    public:
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** ppvObject) override{
            if(iid == IID_IUnknown || iid == __uuidof(IDWriteFontCollectionLoader)){
                AddRef();
                *ppvObject = this;
                return S_OK;
            }
            else {
                *ppvObject = nullptr;
                return E_NOINTERFACE;
            }
        };
        ULONG STDMETHODCALLTYPE AddRef() override{
            return InterlockedIncrement(&refCount);
        };
        ULONG STDMETHODCALLTYPE Release() override{
            ULONG n = InterlockedDecrement(&refCount);
            if(n == 0){
                delete this;
            }
            return n;
        };
        HRESULT CreateEnumeratorFromKey(IDWriteFactory *factory, const void *collectionKey, UINT32 collectionKeySize, IDWriteFontFileEnumerator **fontFileEnumerator) noexcept override {
            OmegaCommon::StrRefBase<wchar_t> path((wchar_t *)collectionKey,collectionKeySize);
            auto ptr = new FontEnumerator(path);
            *fontFileEnumerator = ptr;
            return S_OK;
        }
    };

    class DWriteFont : public Font {
     public:
         Core::UniqueComPtr<IDWriteTextFormat> textFormat;
         DWriteFont(FontDescriptor & desc,IDWriteTextFormat *textFormat):Font(desc),textFormat(textFormat){};
         void * getNativeFont(){
             return (void *)textFormat.get();
         };
         ~DWriteFont(){
             Core::SafeRelease(&textFormat);
         };
     };

    FontEngine * FontEngine::instance;
    class DWriteFontEngineImpl : public FontEngine {
    public:
        Core::UniqueComPtr<ID3D11On12Device> d3d11_device;
        Core::UniqueComPtr<ID3D11DeviceContext> d3d11_devicecontext;
        Core::UniqueComPtr<ID3D12CommandQueue> d3d11_on_12_queue;
        Core::UniqueComPtr<ID2D1Device> d2d1device;
        Core::UniqueComPtr<IDWriteFactory> dwrite_factory;

        SharedHandle<FontLoader> font_loader;

        friend class DWriteTextRect;
        friend class DWriteGlyphRun;
            DWriteFontEngineImpl(){
                  font_loader = std::make_shared<FontLoader>();
                    
                    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),(IUnknown **)&dwrite_factory);
                    if(FAILED(hr)){
                        exit(1);
                    };

                    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
                    auto d3d12_dev = (ID3D12Device *)gte.graphicsEngine->underlyingNativeDevice();

                    D3D12_COMMAND_QUEUE_DESC desc {};
                    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
                    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                    desc.NodeMask = d3d12_dev->GetNodeCount();
                    desc.Priority = 0;
                    hr = d3d12_dev->CreateCommandQueue(&desc,IID_PPV_ARGS(&d3d11_on_12_queue));

                    ID3D11Device *dev;

                    hr = D3D11On12CreateDevice(d3d12_dev,D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG,levels,1,(IUnknown *const *)&d3d11_on_12_queue,1,d3d12_dev->GetNodeCount(),(ID3D11Device **)&dev,&d3d11_devicecontext,nullptr);
                    if(FAILED(hr)){
                        OMEGAWTK_DEBUG("Failed to create D3D11On12");
                        exit(1);
                    }
                    dev->QueryInterface(&d3d11_device);
                    
                    IDXGIDevice *dxgi_dev;

                    dev->QueryInterface(IID_PPV_ARGS(&dxgi_dev));
                    if(FAILED(hr)){
                        OMEGAWTK_DEBUG("Failed to Query DXGI Device from D3D11on12Device");
                        exit(1);
                    }

                    hr = D2D1CreateDevice(dxgi_dev,D2D1::CreationProperties(D2D1_THREADING_MODE_SINGLE_THREADED,D2D1_DEBUG_LEVEL_WARNING,D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS),&d2d1device);
                    if(FAILED(hr)){
                        OMEGAWTK_DEBUG("Failed to Create D2D1 Device");
                        exit(1);
                    }
                    dwrite_factory->RegisterFontCollectionLoader(font_loader.get());
            }
            // Fix for CreateFontA (So MSVC doesn't get confused with our function.)
            #ifdef CreateFont
            #undef CreateFont
            #endif
            
            Core::SharedPtr<Font> CreateFont(FontDescriptor & desc) override {
                        HRESULT hr;
                        IDWriteTextFormat *textFormat;
                        std::wstring w_str;
                        Native::cpp_str_to_cpp_wstr(desc.family,w_str);

                        DWRITE_FONT_WEIGHT weight;
                        DWRITE_FONT_STYLE style;

                        switch (desc.style) {
                            case FontDescriptor::BoldAndItalic : {
                                style = DWRITE_FONT_STYLE_ITALIC;
                                weight = DWRITE_FONT_WEIGHT_BOLD;
                                break;
                            }
                            case FontDescriptor::Bold : {
                                weight = DWRITE_FONT_WEIGHT_BOLD;
                                style = DWRITE_FONT_STYLE_NORMAL;
                                break;
                            }
                            case FontDescriptor::Italic : {
                                weight = DWRITE_FONT_WEIGHT_NORMAL;
                                style = DWRITE_FONT_STYLE_ITALIC;
                                break;
                            }
                            case FontDescriptor::Regular : {
                                weight = DWRITE_FONT_WEIGHT_NORMAL;
                                style = DWRITE_FONT_STYLE_NORMAL;
                                break;
                            };
                        }

                        // Font size stays in DIPs. Physical pixel scaling is
                        // applied by the D2D device context via SetDpi() in
                        // DWriteTextRect, driven by ViewRenderTarget::renderScale.
                        /// TODO: Use Custom Fonts with custom font Collection!
                        hr = dwrite_factory->CreateTextFormat(w_str.c_str(),NULL,weight,style,DWRITE_FONT_STRETCH_NORMAL,FLOAT(desc.size),L"en-us",&textFormat);
                        if(FAILED(hr)){

                        };


                        return std::make_shared<DWriteFont>(desc,textFormat);
            };
            Core::SharedPtr<Font> CreateFontFromFile(OmegaCommon::FS::Path path, FontDescriptor & desc)  override {
                auto path_ustring = OmegaCommon::UniString::fromUTF8(path.str().c_str());
                
                IDWriteFontCollection *collection;
                dwrite_factory->CreateCustomFontCollection(font_loader.get(),path_ustring.getBuffer(),path_ustring.length(),&collection);


                HRESULT hr;
                IDWriteTextFormat *textFormat;
                std::wstring w_str;
                Native::cpp_str_to_cpp_wstr(desc.family,w_str);

                DWRITE_FONT_WEIGHT weight;
                DWRITE_FONT_STYLE style;

                switch (desc.style) {
                    case FontDescriptor::BoldAndItalic : {
                        style = DWRITE_FONT_STYLE_ITALIC;
                        weight = DWRITE_FONT_WEIGHT_BOLD;
                        break;
                    }
                    case FontDescriptor::Bold : {
                        weight = DWRITE_FONT_WEIGHT_BOLD;
                        style = DWRITE_FONT_STYLE_NORMAL;
                        break;
                    }
                    case FontDescriptor::Italic : {
                        weight = DWRITE_FONT_WEIGHT_NORMAL;
                        style = DWRITE_FONT_STYLE_ITALIC;
                        break;
                    }
                    case FontDescriptor::Regular : {
                        weight = DWRITE_FONT_WEIGHT_NORMAL;
                        style = DWRITE_FONT_STYLE_NORMAL;
                        break;
                    };
                }

                // Font size stays in DIPs. See CreateFont() above.
                /// TODO: Use Custom Fonts with custom font Collection!
                hr = dwrite_factory->CreateTextFormat(w_str.c_str(),collection,weight,style,DWRITE_FONT_STRETCH_NORMAL,FLOAT(desc.size),L"en-us",&textFormat);
                if(FAILED(hr)){

                };


                return std::make_shared<DWriteFont>(desc,textFormat);
            };
            ~DWriteFontEngineImpl(){
                dwrite_factory->UnregisterFontCollectionLoader(font_loader.get());
                font_loader.reset();
            }
        };


        FontEngine * FontEngine::inst(){
            return instance;
        };


        void FontEngine::Create(){
            instance = new DWriteFontEngineImpl();
        };


        void FontEngine::Destroy(){
            delete instance;
        };

     class DWriteGlyphRun : public GlyphRun {
     public:
         Core::UniqueComPtr<IDWriteTextLayout> textLayout;
         explicit DWriteGlyphRun(const OmegaCommon::UniString & str, Core::SharedPtr<Font> &font){
             auto *_font = (DWriteFont *)font.get();
             auto FontEngineImpl = (DWriteFontEngineImpl *)FontEngine::inst();
            FontEngineImpl->dwrite_factory->CreateTextLayout((WCHAR *)str.getBuffer(),str.length(),_font->textFormat.get(),0,0,&textLayout);
         }
         Composition::Rect getBoundingRectOfGlyphAtIndex(size_t glyphIdx) override {
            return Composition::Rect {{0.f,0.f},0,0};
         }
     };

    Core::SharedPtr<GlyphRun>
    GlyphRun::fromUStringAndFont(const OmegaCommon::UniString &str, Core::SharedPtr<Font> &font) {
        return Core::SharedPtr<GlyphRun>(new DWriteGlyphRun(str,font));
    }

     class DWriteTextRect : public TextRect {
        DWRITE_PARAGRAPH_ALIGNMENT paraAlignment;
        DWRITE_TEXT_ALIGNMENT textAlignment;
        DWRITE_WORD_WRAPPING wrapping;
        DWRITE_FLOW_DIRECTION flowDirection = DWRITE_FLOW_DIRECTION_LEFT_TO_RIGHT;
        unsigned lineLimit;
        SharedHandle<OmegaGTE::GETexture> target;
        SharedHandle<OmegaGTE::GEFence> fence;
        ID3D11Texture2D *resource{};
        IDXGISurface *surface{};
        ID2D1DeviceContext *context;
     public:
         // Return a null fence: drawRun CPU-waits for the D2D work to
         // complete before returning, so the compositor can treat the
         // texture as ready without cross-queue GPU synchronisation.
         BitmapRes toBitmap() override {
             return {target, nullptr};
         };
         void drawRun(Core::SharedPtr<GlyphRun> &glyphRun,const Composition::Color &color) override {
             auto run = std::dynamic_pointer_cast<DWriteGlyphRun>(glyphRun);
            //  UINT dpi = GetDpiFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
            //  FLOAT scaleFactor = FLOAT(dpi)/96.f;
             run->textLayout->SetMaxWidth(rect.w);
             run->textLayout->SetMaxHeight(rect.h);
             run->textLayout->SetParagraphAlignment(paraAlignment);
             run->textLayout->SetFlowDirection(flowDirection);
             run->textLayout->SetTextAlignment(textAlignment);
             run->textLayout->SetWordWrapping(wrapping);
             if(lineLimit > 0){
                 UINT32 lineCount = 0;
                 HRESULT metricsStatus = run->textLayout->GetLineMetrics(nullptr,0,&lineCount);
                 if((metricsStatus == S_OK || metricsStatus == E_NOT_SUFFICIENT_BUFFER) && lineCount > 0){
                     std::vector<DWRITE_LINE_METRICS> metrics(lineCount);
                     if(SUCCEEDED(run->textLayout->GetLineMetrics(metrics.data(),lineCount,&lineCount))){
                         const UINT32 cappedLineCount = (std::min)(lineLimit,lineCount);
                         FLOAT maxHeight = 0.f;
                         for(UINT32 idx = 0; idx < cappedLineCount; ++idx){
                             maxHeight += metrics[idx].height;
                         }
                         if(maxHeight > 0.f){
                             run->textLayout->SetMaxHeight(maxHeight);
                         }
                     }
                 }
             }
               auto *FontEngineImpl = dynamic_cast<DWriteFontEngineImpl *>(FontEngine::inst());
               FontEngineImpl->d3d11_device->AcquireWrappedResources((ID3D11Resource *const *)&resource,1);

             context->BeginDraw();
             context->Clear(D2D1::ColorF(0.f,0.f,0.f,0.f));
             ID2D1Brush * brush = nullptr;
             context->CreateSolidColorBrush(D2D1::ColorF(color.r,color.g,color.b,color.a),(ID2D1SolidColorBrush **)&brush);
             context->DrawTextLayout(D2D1::Point2F(0,0),run->textLayout.get(),brush,D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
             HRESULT hr = context->EndDraw();

              FontEngineImpl->d3d11_device->ReleaseWrappedResources((ID3D11Resource *const *)&resource,1);

             FontEngineImpl->d3d11_devicecontext->Flush();

            auto native_fence = (ID3D12Fence *)fence->native();

            // Signal the fence from the D3D11On12 queue so we can block
            // the CPU until the D2D rasterisation has fully committed to
            // the wrapped D3D12 resource. Using a CPU wait rather than a
            // cross-queue GEFence wait avoids two problems:
            //   1. The external Signal bypasses GED3D12Fence's cached
            //      lastSignaledValue, so GECommandQueue::notifyCommandBuffer
            //      would silently skip its commandQueue->Wait.
            //   2. Returning a null fence from toBitmap lets the compositor
            //      path mirror Metal exactly (no endRenderPass churn).
            FontEngineImpl->d3d11_on_12_queue->Signal(native_fence,1);
            if(native_fence->GetCompletedValue() < 1){
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if(ev != nullptr){
                    native_fence->SetEventOnCompletion(1, ev);
                    WaitForSingleObject(ev, INFINITE);
                    CloseHandle(ev);
                }
            }

             Core::SafeRelease(&brush);
             if(hr == D2DERR_RECREATE_TARGET){
                 Core::SafeRelease(&context);
                 D2D1CreateDeviceContext(surface,D2D1::CreationProperties(D2D1_THREADING_MODE_MULTI_THREADED,D2D1_DEBUG_LEVEL_WARNING,D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS),&context);
             }
         }
         void * getNative() override{
            return nullptr;
         };
         explicit DWriteTextRect(Composition::Rect & rect,const TextLayoutDescriptor & layoutDesc, float renderScale):
         TextRect(rect), 
         lineLimit(layoutDesc.lineLimit),
         target(nullptr),
         context(nullptr){
             this->renderScale = renderScale > 0.f ? renderScale : 1.f;
             HRESULT hr;

             

             switch (layoutDesc.alignment) {
                case TextLayoutDescriptor::LeftUpper : {
                    textAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
                    paraAlignment = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
                    break;
                }
                case TextLayoutDescriptor::LeftCenter : {
                    textAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
                    paraAlignment = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
                    break;
                }
                case TextLayoutDescriptor::LeftLower : {
                    textAlignment = DWRITE_TEXT_ALIGNMENT_LEADING;
                    paraAlignment = DWRITE_PARAGRAPH_ALIGNMENT_FAR;
                    break;
                }
                case TextLayoutDescriptor::MiddleUpper : {
                    textAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
                    paraAlignment = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
                    break;
                }
                case TextLayoutDescriptor::MiddleCenter : {
                    textAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
                    paraAlignment = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
                    break;
                }
                case TextLayoutDescriptor::MiddleLower : {
                    textAlignment = DWRITE_TEXT_ALIGNMENT_CENTER;
                    paraAlignment = DWRITE_PARAGRAPH_ALIGNMENT_FAR;
                    break;
                }
                case TextLayoutDescriptor::RightUpper : {
                    textAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
                    paraAlignment = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
                    break;
                }
                case TextLayoutDescriptor::RightCenter : {
                    textAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
                    paraAlignment = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
                    break;
                }
                case TextLayoutDescriptor::RightLower : {
                    textAlignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
                    paraAlignment = DWRITE_PARAGRAPH_ALIGNMENT_FAR;
                    break;
                }
             }

             switch (layoutDesc.wrapping) {
                case TextLayoutDescriptor::WrapByWord : {
                    wrapping = DWRITE_WORD_WRAPPING_WRAP;
                    break;
                }
                case TextLayoutDescriptor::WrapByCharacter : {
                    wrapping = DWRITE_WORD_WRAPPING_CHARACTER;
                    break;
                }
                case TextLayoutDescriptor::None :
                default : {
                    wrapping = DWRITE_WORD_WRAPPING_NO_WRAP;
                    break;
                }
             }

             OmegaGTE::TextureDescriptor textureDesc {OmegaGTE::GETexture::Texture2D};
             textureDesc.usage = OmegaGTE::GETexture::RenderTarget;
             textureDesc.pixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm;
             textureDesc.height = (unsigned)(rect.h * this->renderScale);
             textureDesc.width = (unsigned)(rect.w * this->renderScale);

             target = gte.graphicsEngine->makeTexture(textureDesc);
             
             D3D11_RESOURCE_FLAGS fgs{};
             fgs.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
             fgs.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
             fgs.CPUAccessFlags = D3D11_USAGE_DEFAULT;
             fgs.StructureByteStride = 0.f;

             OMEGAWTK_DEBUG("Ok! 1");

              auto FontEngineImpl = (DWriteFontEngineImpl *)FontEngine::inst();

             // OutState intentionally stays at RENDER_TARGET: that's the
             // state GED3D12Texture caches for a usage=RenderTarget texture
             // (see gte/src/d3d12/GED3D12.cpp makeTexture). If OutState were
             // PIXEL_SHADER_RESOURCE, 11on12 would transition the actual
             // D3D12 state but GED3D12Texture::currentState would stay
             // RENDER_TARGET, and the next bindResourceAtFragmentShader
             // would emit a malformed RENDER_TARGET→PIXEL_SHADER_RESOURCE
             // barrier against a resource already in PIXEL_SHADER_RESOURCE.
             // Leaving it in RENDER_TARGET lets the compositor's normal
             // bind path issue a correct transition barrier.
             hr = FontEngineImpl->d3d11_device->CreateWrappedResource((IUnknown *)target->native(),&fgs,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,IID_PPV_ARGS(&resource));
             if(FAILED(hr)){
                 OMEGAWTK_DEBUG("Failed to Create Wrapped Resource. ERR:" << std::hex << hr << std::dec);
                 exit(1);
             }

              OMEGAWTK_DEBUG("Ok! 2");
             hr = resource->QueryInterface(IID_PPV_ARGS(&surface));
             if(FAILED(hr)){
                  OMEGAWTK_DEBUG("Failed to get DXGISurface. ERR:" << std::hex << hr << std::dec);
                 exit(1);
             }

             OMEGAWTK_DEBUG("Ok! 3");
             hr = FontEngineImpl->d2d1device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS,&context);
             if(FAILED(hr)){
                  OMEGAWTK_DEBUG("Failed to create D2D1DeviceContext ERR:" << std::hex << hr << std::dec);
                 exit(1);
             }

             OMEGAWTK_DEBUG("Ok! 4");

             ID2D1Bitmap1 *bitmap;

             // Stamp the bitmap with 96 * renderScale DPI so its *logical*
             // (DIP) size equals rect.w × rect.h — matching the DIP-valued
             // text layout box and the DIP-valued font size. Without this
             // the bitmap defaults to 96 DPI, so its logical size becomes
             // rect.w*scale × rect.h*scale DIPs and DrawTextLayout with
             // MaxWidth=rect.w only fills the top-left corner.
            hr = context->CreateBitmapFromDxgiSurface(surface,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96.f * this->renderScale,96.f * this->renderScale),&bitmap);
             
            if(FAILED(hr)){
                  OMEGAWTK_DEBUG("Failed to create Bitmap from DXGISurface ERR:" << std::hex << hr << std::dec);
                 exit(1);
             }
             context->SetTarget(bitmap);

             OMEGAWTK_DEBUG("Ok! 5");

             context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
             // No explicit transform: the bitmap is already authored at
             // 96*renderScale DPI, so draw calls in DIPs are rasterised
             // into physical pixels automatically. Adding a SetTransform
             // scale on top would double-scale.
             context->SetTransform(D2D1::Matrix3x2F::Identity());
              OMEGAWTK_DEBUG("DWriteTextRect Successfully Created");

              fence = gte.graphicsEngine->makeFence();
         };
         ~DWriteTextRect() override {
             Core::SafeRelease(&surface);
            Core::SafeRelease(&context);
         }
     };

     SharedHandle<TextRect> TextRect::Create(Composition::Rect rect,const TextLayoutDescriptor & layoutDesc, float renderScale){
         return SharedHandle<TextRect>(new DWriteTextRect(rect,layoutDesc,renderScale));
     };
    }

