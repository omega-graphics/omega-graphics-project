#include "omegaWTK/Composition/FontEngine.h"
#include "omegaWTK/Composition/TextLayoutEngine.h"
#include "omegaWTK/Core/GTEHandle.h"
#include "NativePrivate/win/WinUtils.h"
#include "omegaWTK/Core/Microsoft.h"
#include "omega-common/unicode.h"
#include "omega-common/assets.h"
#include "../GlyphAtlas.h"

#include <dwrite.h>
#include <dwrite_1.h>
// IDWriteFactory2 / IDWriteFontFallback — Text-Layout-Engine-Plan Phase 4
// fallback driver. Available on Windows 8.1+, which is below WTK's
// supported floor.
#include <dwrite_2.h>

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

#ifdef OMEGAWTK_HAVE_MSDFGEN
#include <msdfgen.h>
#include <core/edge-coloring.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>

namespace OmegaWTK::Composition {

    namespace {
        /// OMEGAWTK_TRACE_TEXT=1 turns on the Phase-6.7 text-pipeline
        /// trace logs (probe results, MSDF tile dumps). Mirrors the
        /// Linux backend so the same env var lights up both sides.
        bool textTraceEnabled() {
            static const bool enabled = []() {
                const char *e = std::getenv("OMEGAWTK_TRACE_TEXT");
                return e != nullptr && e[0] != '\0' && e[0] != '0';
            }();
            return enabled;
        }

        /// 0 = upper, 1 = center, 2 = lower. Mirrors the Linux helper.
        int verticalAlignmentCategory(TextLayoutDescriptor::Alignment alignment){
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

#ifdef OMEGAWTK_HAVE_MSDFGEN
        /// Square 32×32 MSDF tile size + 4-px distance range. Same
        /// constants as the Linux backend (HarfbuzzFontEngine.cpp).
        constexpr int    kMsdfTileSize = 32;
        constexpr double kMsdfRange    = 4.0;

        /// IDWriteGeometrySink that records the DWrite outline into a
        /// `msdfgen::Shape`. DWrite emits outlines in design-space at the
        /// emSize passed to `GetGlyphRunOutline` with Y growing *down*;
        /// msdfgen expects Y growing *up* (its `orientContours()` is
        /// calibrated against TrueType's CW-outer convention measured in
        /// Y-up space). We flip the sign at point-build time so the
        /// rest of the msdfgen pipeline is identical to the Linux path.
        class MsdfGeometrySink final : public IDWriteGeometrySink {
            ULONG refCount_ = 1;
            msdfgen::Shape *shape_ = nullptr;
            msdfgen::Contour *contour_ = nullptr;
            msdfgen::Point2 lastPoint_ {0.0, 0.0};
            // Start point of the current figure, captured at BeginFigure
            // so EndFigure(CLOSED) can stitch the closing edge — DWrite
            // does not emit that edge explicitly. Mirrors the macOS
            // CGPathMsdfContext.contourStart handling.
            msdfgen::Point2 figureStart_ {0.0, 0.0};
            UINT32 segmentCount_ = 0;
        public:
            explicit MsdfGeometrySink(msdfgen::Shape *shape) : shape_(shape) {}

            /// Diagnostic: how many edges landed in the shape. The probe
            /// path uses this to distinguish "outline exists but is
            /// empty" (bitmap-only face) from a successful walk.
            UINT32 segmentCount() const { return segmentCount_; }

            // ID2D1SimplifiedGeometrySink methods (all void except Close).
            STDMETHOD_(void, SetFillMode)(D2D1_FILL_MODE) override {}
            STDMETHOD_(void, SetSegmentFlags)(D2D1_PATH_SEGMENT) override {}
            STDMETHOD_(void, BeginFigure)(D2D1_POINT_2F startPoint,
                                          D2D1_FIGURE_BEGIN) override {
                if(shape_ == nullptr) return;
                contour_ = &shape_->addContour();
                lastPoint_   = msdfgen::Point2(startPoint.x, -startPoint.y);
                figureStart_ = lastPoint_;
            }
            STDMETHOD_(void, AddLines)(const D2D1_POINT_2F *points,
                                       UINT32 pointsCount) override {
                if(contour_ == nullptr) return;
                for(UINT32 i = 0; i < pointsCount; ++i){
                    msdfgen::Point2 endpoint(points[i].x, -points[i].y);
                    if(endpoint != lastPoint_){
                        contour_->addEdge(msdfgen::EdgeHolder(lastPoint_, endpoint));
                        lastPoint_ = endpoint;
                        ++segmentCount_;
                    }
                }
            }
            STDMETHOD_(void, AddBeziers)(const D2D1_BEZIER_SEGMENT *beziers,
                                         UINT32 beziersCount) override {
                if(contour_ == nullptr) return;
                for(UINT32 i = 0; i < beziersCount; ++i){
                    msdfgen::Point2 c1(beziers[i].point1.x, -beziers[i].point1.y);
                    msdfgen::Point2 c2(beziers[i].point2.x, -beziers[i].point2.y);
                    msdfgen::Point2 endpoint(beziers[i].point3.x, -beziers[i].point3.y);
                    contour_->addEdge(
                        msdfgen::EdgeHolder(lastPoint_, c1, c2, endpoint));
                    lastPoint_ = endpoint;
                    ++segmentCount_;
                }
            }
            STDMETHOD_(void, EndFigure)(D2D1_FIGURE_END figureEnd) override {
                // Stitch the closing edge for closed figures. DWrite
                // (like Core Text) does not emit the close edge as an
                // AddLines / AddBeziers call — it sets FIGURE_END_CLOSED
                // and expects the consumer to draw back to BeginFigure's
                // start point on its own. Without this, msdfgen sees an
                // *open* contour and the signed-distance sign for the
                // enclosed region is undefined; multi-contour glyphs
                // (m, p, o, d, g, i, e, …) render with solid rectangular
                // artifacts overlaid on the silhouette because the
                // would-be-enclosed area gets misclassified.
                if(contour_ != nullptr &&
                   figureEnd == D2D1_FIGURE_END_CLOSED &&
                   lastPoint_ != figureStart_){
                    contour_->addEdge(msdfgen::EdgeHolder(lastPoint_, figureStart_));
                    lastPoint_ = figureStart_;
                    ++segmentCount_;
                }
                contour_ = nullptr;
            }
            STDMETHOD(Close)() override { return S_OK; }

            // IUnknown.
            STDMETHOD(QueryInterface)(REFIID iid, void **ppv) override {
                if(iid == IID_IUnknown ||
                   iid == __uuidof(ID2D1SimplifiedGeometrySink) ||
                   iid == __uuidof(IDWriteGeometrySink)){
                    AddRef();
                    *ppv = this;
                    return S_OK;
                }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            STDMETHOD_(ULONG, AddRef)() override {
                return InterlockedIncrement(&refCount_);
            }
            STDMETHOD_(ULONG, Release)() override {
                ULONG n = InterlockedDecrement(&refCount_);
                if(n == 0){ delete this; }
                return n;
            }
        };
#endif // OMEGAWTK_HAVE_MSDFGEN

        /// Minimal IDWriteTextAnalysisSource. We feed the analyzer one
        /// contiguous UTF-16 buffer with a single locale and LTR reading
        /// direction; the Phase-6.7-c3 shape path doesn't try to honor
        /// per-character locale or number substitution (that arrives
        /// with the layout-engine plan).
        class DWriteAnalysisSource final : public IDWriteTextAnalysisSource {
            ULONG refCount_ = 1;
            const WCHAR *text_ = nullptr;
            UINT32 length_ = 0;
            const WCHAR *locale_ = L"en-us";
        public:
            DWriteAnalysisSource(const WCHAR *text, UINT32 length, const WCHAR *locale)
                : text_(text), length_(length), locale_(locale) {}

            STDMETHOD(GetTextAtPosition)(UINT32 pos,
                                         WCHAR const **outText,
                                         UINT32 *outLen) override {
                if(pos >= length_){
                    *outText = nullptr;
                    *outLen = 0;
                    return S_OK;
                }
                *outText = text_ + pos;
                *outLen = length_ - pos;
                return S_OK;
            }
            STDMETHOD(GetTextBeforePosition)(UINT32 pos,
                                             WCHAR const **outText,
                                             UINT32 *outLen) override {
                if(pos == 0 || pos > length_){
                    *outText = nullptr;
                    *outLen = 0;
                    return S_OK;
                }
                *outText = text_;
                *outLen = pos;
                return S_OK;
            }
            STDMETHOD_(DWRITE_READING_DIRECTION, GetParagraphReadingDirection)() override {
                return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
            }
            STDMETHOD(GetLocaleName)(UINT32 pos,
                                     UINT32 *outLen,
                                     WCHAR const **localeName) override {
                *outLen = (pos < length_) ? (length_ - pos) : 0;
                *localeName = locale_;
                return S_OK;
            }
            STDMETHOD(GetNumberSubstitution)(UINT32 pos,
                                             UINT32 *outLen,
                                             IDWriteNumberSubstitution **sub) override {
                *outLen = (pos < length_) ? (length_ - pos) : 0;
                *sub = nullptr;
                return S_OK;
            }

            STDMETHOD(QueryInterface)(REFIID iid, void **ppv) override {
                if(iid == IID_IUnknown ||
                   iid == __uuidof(IDWriteTextAnalysisSource)){
                    AddRef();
                    *ppv = this;
                    return S_OK;
                }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            STDMETHOD_(ULONG, AddRef)() override {
                return InterlockedIncrement(&refCount_);
            }
            STDMETHOD_(ULONG, Release)() override {
                ULONG n = InterlockedDecrement(&refCount_);
                if(n == 0){ delete this; }
                return n;
            }
        };

        /// IDWriteTextAnalysisSink that just records every
        /// `SetScriptAnalysis` callback verbatim. We don't ask for line
        /// breakpoints, BiDi, or number substitution in shape() — we
        /// only run `AnalyzeScript` — so the other sink methods are
        /// no-ops that return S_OK.
        struct DWriteScriptRange {
            UINT32 textPosition = 0;
            UINT32 textLength = 0;
            DWRITE_SCRIPT_ANALYSIS analysis {};
        };
        class DWriteAnalysisSink final : public IDWriteTextAnalysisSink {
            ULONG refCount_ = 1;
        public:
            std::vector<DWriteScriptRange> scriptRanges;

            STDMETHOD(SetScriptAnalysis)(UINT32 pos, UINT32 len,
                                         const DWRITE_SCRIPT_ANALYSIS *sa) override {
                DWriteScriptRange r;
                r.textPosition = pos;
                r.textLength = len;
                r.analysis = *sa;
                scriptRanges.push_back(r);
                return S_OK;
            }
            STDMETHOD(SetLineBreakpoints)(UINT32, UINT32,
                                          const DWRITE_LINE_BREAKPOINT *) override {
                return S_OK;
            }
            STDMETHOD(SetBidiLevel)(UINT32, UINT32, UINT8, UINT8) override {
                return S_OK;
            }
            STDMETHOD(SetNumberSubstitution)(UINT32, UINT32,
                                             IDWriteNumberSubstitution *) override {
                return S_OK;
            }

            STDMETHOD(QueryInterface)(REFIID iid, void **ppv) override {
                if(iid == IID_IUnknown ||
                   iid == __uuidof(IDWriteTextAnalysisSink)){
                    AddRef();
                    *ppv = this;
                    return S_OK;
                }
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            STDMETHOD_(ULONG, AddRef)() override {
                return InterlockedIncrement(&refCount_);
            }
            STDMETHOD_(ULONG, Release)() override {
                ULONG n = InterlockedDecrement(&refCount_);
                if(n == 0){ delete this; }
                return n;
            }
        };

        /// Translate FontDescriptor → DWrite weight/style. Used by
        /// font construction AND face resolution; keeping it as a
        /// helper avoids drift between the two switches that previously
        /// inlined the mapping.
        void descToDWriteStyle(const FontDescriptor &desc,
                               DWRITE_FONT_WEIGHT &weight,
                               DWRITE_FONT_STYLE &style) {
            switch(desc.style){
                case FontDescriptor::BoldAndItalic:
                    weight = DWRITE_FONT_WEIGHT_BOLD;
                    style  = DWRITE_FONT_STYLE_ITALIC;
                    break;
                case FontDescriptor::Bold:
                    weight = DWRITE_FONT_WEIGHT_BOLD;
                    style  = DWRITE_FONT_STYLE_NORMAL;
                    break;
                case FontDescriptor::Italic:
                    weight = DWRITE_FONT_WEIGHT_NORMAL;
                    style  = DWRITE_FONT_STYLE_ITALIC;
                    break;
                case FontDescriptor::Regular:
                default:
                    weight = DWRITE_FONT_WEIGHT_NORMAL;
                    style  = DWRITE_FONT_STYLE_NORMAL;
                    break;
            }
        }

        /// Resolve a DWrite `IDWriteFontFace` for the requested family
        /// + weight + style from the given collection. Returns nullptr
        /// when the family isn't present in the collection. Caller owns
        /// the returned reference.
        IDWriteFontFace * resolveFontFace(IDWriteFontCollection *collection,
                                          const FontDescriptor &desc) {
            if(collection == nullptr){ return nullptr; }
            DWRITE_FONT_WEIGHT weight;
            DWRITE_FONT_STYLE style;
            descToDWriteStyle(desc, weight, style);

            std::wstring family;
            Native::cpp_str_to_cpp_wstr(desc.family, family);

            UINT32 idx = 0;
            BOOL exists = FALSE;
            if(FAILED(collection->FindFamilyName(family.c_str(), &idx, &exists)) || !exists){
                return nullptr;
            }
            IDWriteFontFamily *fam = nullptr;
            if(FAILED(collection->GetFontFamily(idx, &fam)) || fam == nullptr){
                return nullptr;
            }
            IDWriteFont *font = nullptr;
            HRESULT hr = fam->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL,
                                                   style, &font);
            Core::SafeRelease(&fam);
            if(FAILED(hr) || font == nullptr){
                return nullptr;
            }
            IDWriteFontFace *face = nullptr;
            hr = font->CreateFontFace(&face);
            Core::SafeRelease(&font);
            if(FAILED(hr)){
                return nullptr;
            }
            return face;
        }
    } // anonymous namespace

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

    // ---- Asset-bundle font loading (Phase 6 follow-up) -----------------
    //
    // CreateFontFromAsset reads a font file's bytes from an AssetBundle
    // and feeds them into DWrite *without* writing the blob to a temp
    // file. The plumbing has three pieces:
    //
    //   * MemoryFontFileStream  — IDWriteFontFileStream over a shared
    //                             vector<uint8_t> blob. DWrite reads
    //                             the file's bytes through this.
    //   * MemoryFontFileLoader  — IDWriteFontFileLoader. Owns a registry
    //                             of blobs keyed by uint64_t; DWrite
    //                             invokes its CreateStreamFromKey with
    //                             a key handed back by registerBlob().
    //   * MemoryFontEnumerator + MemoryFontCollectionLoader — produce a
    //                             single-file IDWriteFontFileEnumerator
    //                             whose one file is built via the
    //                             memory file loader. Lets us drive
    //                             CreateCustomFontCollection with the
    //                             same shape that the path-based
    //                             FontLoader uses.
    //
    // The blob's lifetime is owned by the file loader's registry — the
    // blob stays alive as long as the loader keeps the entry, which is
    // for the lifetime of the engine (releases happen in dtor when the
    // loader is unregistered). This mirrors how the existing FontLoader
    // / FontEnumerator handle their path-string keys.

    class MemoryFontFileStream final : public IDWriteFontFileStream {
        ULONG refCount_ = 1;
        std::shared_ptr<std::vector<std::uint8_t>> blob_;
    public:
        explicit MemoryFontFileStream(std::shared_ptr<std::vector<std::uint8_t>> blob)
            : blob_(std::move(blob)) {}

        HRESULT STDMETHODCALLTYPE ReadFileFragment(
                const void **fragmentStart,
                UINT64 fileOffset, UINT64 fragmentSize,
                void **fragmentContext) noexcept override {
            if(blob_ == nullptr || fragmentStart == nullptr || fragmentContext == nullptr){
                return E_FAIL;
            }
            const UINT64 sz = blob_->size();
            if(fileOffset > sz || fragmentSize > sz - fileOffset){
                return E_FAIL;
            }
            *fragmentStart = blob_->data() + fileOffset;
            // No allocated context — the blob owns the bytes; ReleaseFileFragment
            // is a no-op.
            *fragmentContext = nullptr;
            return S_OK;
        }
        void STDMETHODCALLTYPE ReleaseFileFragment(void *) noexcept override {}
        HRESULT STDMETHODCALLTYPE GetFileSize(UINT64 *fileSize) noexcept override {
            if(fileSize == nullptr) return E_POINTER;
            *fileSize = (blob_ != nullptr) ? blob_->size() : 0;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetLastWriteTime(UINT64 *lastWriteTime) noexcept override {
            if(lastWriteTime == nullptr) return E_POINTER;
            // In-memory blob has no on-disk timestamp; reporting 0 is
            // legal per the DWrite docs ("if the time is unknown").
            *lastWriteTime = 0;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **ppv) noexcept override {
            if(iid == IID_IUnknown || iid == __uuidof(IDWriteFontFileStream)){
                AddRef();
                *ppv = this;
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() noexcept override {
            return InterlockedIncrement(&refCount_);
        }
        ULONG STDMETHODCALLTYPE Release() noexcept override {
            ULONG n = InterlockedDecrement(&refCount_);
            if(n == 0) delete this;
            return n;
        }
    };

    class MemoryFontFileLoader final : public IDWriteFontFileLoader {
        ULONG refCount_ = 1;
        std::unordered_map<std::uint64_t,
                           std::shared_ptr<std::vector<std::uint8_t>>> blobs_;
        std::uint64_t nextKey_ = 1;  // 0 reserved for "invalid"
    public:
        /// Register a blob; returns the key DWrite will hand back via
        /// CreateStreamFromKey. The blob is kept alive by the loader
        /// until the loader itself is destroyed.
        std::uint64_t registerBlob(std::shared_ptr<std::vector<std::uint8_t>> blob){
            std::uint64_t key = nextKey_++;
            blobs_.emplace(key, std::move(blob));
            return key;
        }

        HRESULT STDMETHODCALLTYPE CreateStreamFromKey(
                void const *fontFileReferenceKey,
                UINT32 fontFileReferenceKeySize,
                IDWriteFontFileStream **fontFileStream) noexcept override {
            if(fontFileReferenceKey == nullptr ||
               fontFileReferenceKeySize != sizeof(std::uint64_t) ||
               fontFileStream == nullptr){
                return E_INVALIDARG;
            }
            std::uint64_t key;
            std::memcpy(&key, fontFileReferenceKey, sizeof(key));
            auto it = blobs_.find(key);
            if(it == blobs_.end()){
                return E_FAIL;
            }
            *fontFileStream = new MemoryFontFileStream(it->second);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **ppv) noexcept override {
            if(iid == IID_IUnknown || iid == __uuidof(IDWriteFontFileLoader)){
                AddRef();
                *ppv = this;
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() noexcept override {
            return InterlockedIncrement(&refCount_);
        }
        ULONG STDMETHODCALLTYPE Release() noexcept override {
            ULONG n = InterlockedDecrement(&refCount_);
            if(n == 0) delete this;
            return n;
        }
    };

    // Single-file enumerator: returns one IDWriteFontFile (built against
    // the memory file loader) and then reports "no more files". Mirrors
    // the path-based FontEnumerator shape.
    class MemoryFontEnumerator final : public IDWriteFontFileEnumerator {
        ULONG refCount_ = 1;
        IDWriteFactory *factory_;
        MemoryFontFileLoader *fileLoader_;
        std::uint64_t blobKey_;
        bool yielded_ = false;
    public:
        MemoryFontEnumerator(IDWriteFactory *factory,
                             MemoryFontFileLoader *fileLoader,
                             std::uint64_t blobKey)
            : factory_(factory), fileLoader_(fileLoader), blobKey_(blobKey) {
            if(fileLoader_ != nullptr) fileLoader_->AddRef();
        }
        ~MemoryFontEnumerator(){
            if(fileLoader_ != nullptr) fileLoader_->Release();
        }

        HRESULT STDMETHODCALLTYPE MoveNext(BOOL *hasCurrentFile) noexcept override {
            if(hasCurrentFile == nullptr) return E_POINTER;
            *hasCurrentFile = yielded_ ? FALSE : TRUE;
            yielded_ = true;
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile **fontFile) noexcept override {
            if(fontFile == nullptr || factory_ == nullptr || fileLoader_ == nullptr){
                return E_FAIL;
            }
            return factory_->CreateCustomFontFileReference(
                &blobKey_, sizeof(blobKey_),
                fileLoader_,
                fontFile);
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **ppv) noexcept override {
            if(iid == IID_IUnknown || iid == __uuidof(IDWriteFontFileEnumerator)){
                AddRef();
                *ppv = this;
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() noexcept override {
            return InterlockedIncrement(&refCount_);
        }
        ULONG STDMETHODCALLTYPE Release() noexcept override {
            ULONG n = InterlockedDecrement(&refCount_);
            if(n == 0) delete this;
            return n;
        }
    };

    class MemoryFontCollectionLoader final : public IDWriteFontCollectionLoader {
        ULONG refCount_ = 1;
        MemoryFontFileLoader *fileLoader_;
    public:
        explicit MemoryFontCollectionLoader(MemoryFontFileLoader *fileLoader)
            : fileLoader_(fileLoader) {
            if(fileLoader_ != nullptr) fileLoader_->AddRef();
        }
        ~MemoryFontCollectionLoader(){
            if(fileLoader_ != nullptr) fileLoader_->Release();
        }

        HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(
                IDWriteFactory *factory,
                void const *collectionKey, UINT32 collectionKeySize,
                IDWriteFontFileEnumerator **fontFileEnumerator) noexcept override {
            if(collectionKey == nullptr ||
               collectionKeySize != sizeof(std::uint64_t) ||
               fontFileEnumerator == nullptr || fileLoader_ == nullptr){
                return E_INVALIDARG;
            }
            std::uint64_t blobKey;
            std::memcpy(&blobKey, collectionKey, sizeof(blobKey));
            *fontFileEnumerator = new MemoryFontEnumerator(factory, fileLoader_, blobKey);
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **ppv) noexcept override {
            if(iid == IID_IUnknown || iid == __uuidof(IDWriteFontCollectionLoader)){
                AddRef();
                *ppv = this;
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() noexcept override {
            return InterlockedIncrement(&refCount_);
        }
        ULONG STDMETHODCALLTYPE Release() noexcept override {
            ULONG n = InterlockedDecrement(&refCount_);
            if(n == 0) delete this;
            return n;
        }
    };

    class DWriteFont : public Font {
     public:
         Core::UniqueComPtr<IDWriteTextFormat> textFormat;
         /// Resolved IDWriteFontFace for `desc`. Held alongside the
         /// IDWriteTextFormat so the Phase-6.7 MSDF path can call
         /// `GetGlyphRunOutline` / `GetDesignGlyphMetrics` directly,
         /// and so `shape()` can compare the GetGlyphs face to the
         /// requested face to detect fallback. May be null on faces
         /// that failed to resolve (e.g. family not installed).
         Core::UniqueComPtr<IDWriteFontFace> fontFace;
         DWriteFont(FontDescriptor & desc, IDWriteTextFormat *textFormat,
                    IDWriteFontFace *face)
             :Font(desc), textFormat(textFormat), fontFace(face){};
         void * getNativeFont(){
             return (void *)textFormat.get();
         };
         /// Phase-6.7 accessor used by `shape()` for the face-identity
         /// fallback check and by the MSDF rasterize lambda for outline
         /// extraction.
         IDWriteFontFace * getFontFace() const { return fontFace.comPtr.Get(); }

         /// Expose the protected mode setter so the engine factory can
         /// flip the font to MSDF once the outline probe succeeds.
         using Font::setMode;

         // Text-Layout-Engine-Plan.md Phase 6: per-font metrics for the
         // WTK-owned layout engine. Sourced from `IDWriteFontFace::
         // GetMetrics` (design units) and converted to canvas pixels at
         // the descriptor's emSize — the same logical-pixel space the
         // MSDF atlas's outline pass walks. The layout engine uses
         // these for line stride + baseline placement; zero metrics
         // collapse multi-line text onto one stripe, which is the
         // correct degenerate behavior when the face couldn't be
         // resolved (the `BitmapFallback` path then handles drawing).
         FontMetrics getMetrics() const override {
             FontMetrics m;
             IDWriteFontFace *face = fontFace.comPtr.Get();
             if(face == nullptr){
                 return m;
             }
             DWRITE_FONT_METRICS fm {};
             face->GetMetrics(&fm);
             const double upem = static_cast<double>(fm.designUnitsPerEm);
             if(upem <= 0.0){
                 return m;
             }
             const double emSize = static_cast<double>(desc.size);
             const double duToPx = emSize / upem;
             m.ascent  = static_cast<float>(fm.ascent  * duToPx);
             m.descent = static_cast<float>(fm.descent * duToPx);
             m.lineGap = static_cast<float>(fm.lineGap * duToPx);
             if(m.lineGap < 0.f) m.lineGap = 0.f;
             return m;
         }
         ~DWriteFont(){
             Core::SafeRelease(&textFormat);
             // fontFace is released by UniqueComPtr's dtor.
         };
     };

    FontEngine * FontEngine::instance;

    class DWriteFontEngineImpl;

    // Text-Layout-Engine-Plan.md Phase 6 — DWrite-backed `ITextShaper`.
    //
    // Shapes one logical run at a time via `IDWriteTextAnalyzer`'s
    // low-level GetGlyphs + GetGlyphPlacements pair. We do *not* drive
    // `IDWriteTextLayout` (which would impose its own layout / wrap /
    // alignment decisions — exactly what Phase 6 moves out of platform
    // hands). DWrite's shaper still produces kerning + ligatures + mark
    // positioning; the layout engine owns line composition, baseline
    // placement, and alignment.
    //
    // Output contract: one `ShaperGlyph` per output glyph (already
    // post-kerning / -ligature), in visual order. `advance` is per-
    // glyph from `GetGlyphPlacements`; `xOffset / yOffset` are mark-
    // positioning deltas from DWRITE_GLYPH_OFFSET (advanceOffset is
    // along the baseline, ascenderOffset is positive-up — flipped to
    // our Y-down convention). `cluster` is derived from clusterMap[]
    // (text-index → first-glyph-index inversion).
    class DWriteShaper : public ITextShaper {
    public:
        // Body defined out-of-line below — needs DWriteFontEngineImpl's
        // complete type to reach the IDWriteFactory for analyzer
        // creation.
        OmegaCommon::Vector<ShaperGlyph> shapeRun(const ShaperInput & input) override;
    };

    // Text-Layout-Engine-Plan.md Phase 6 — DWrite-backed `IFontFallback`.
    //
    // Uses `IDWriteFontFallback::MapCharacters` (the documented public
    // API for accessing the system fallback chain on Windows 8.1+) to
    // ask DWrite: "what installed face will render this codepoint?"
    // The substitute is materialized through the engine's normal
    // `CreateFont` path so it goes through the MSDF probe and ends up
    // with a real `DWriteFont` plus an MSDF-or-bitmap atlas.
    //
    // Cache key is the substitute face's family name — repeated
    // fallback for codepoints serviced by the same face (e.g. every
    // CJK ideograph in a string falls back to "MS Gothic") returns
    // one shared `Font` and one shared atlas.
    class DWriteFontFallback : public IFontFallback {
        DWriteFontEngineImpl *engine_ = nullptr;
        std::unordered_map<std::string, Core::SharedPtr<Font>> byFamily_;
    public:
        explicit DWriteFontFallback(DWriteFontEngineImpl *engine)
            : engine_(engine) {}

        Core::SharedPtr<Font> fallbackForCodepoint(
            Core::SharedPtr<Font> requested,
            std::uint32_t codepoint) override;
    };

    class DWriteFontEngineImpl : public FontEngine {
    public:
        Core::UniqueComPtr<ID3D11On12Device> d3d11_device;
        Core::UniqueComPtr<ID3D11DeviceContext> d3d11_devicecontext;
        Core::UniqueComPtr<ID3D12CommandQueue> d3d11_on_12_queue;
        Core::UniqueComPtr<ID2D1Device> d2d1device;
        Core::UniqueComPtr<IDWriteFactory> dwrite_factory;
        // IDWriteFactory2 cached for the fallback driver
        // (GetSystemFontFallback is a Factory2 method). May be null on
        // pre-Win8.1 hosts; `DWriteFontFallback` then returns nullptr
        // and the layout engine leaves `.notdef` clusters alone.
        Core::UniqueComPtr<IDWriteFactory2> dwrite_factory2;

        SharedHandle<FontLoader> font_loader;
        // Asset-bundle font loading (see MemoryFontFileLoader / etc).
        // Both loaders are reference-counted COM objects; we hold a raw
        // pointer + a manual AddRef in the ctor + Release in the dtor
        // to mirror the COM lifetime model. The collection loader keeps
        // its own ref on the file loader, so the file loader survives
        // even after we Release ours — until the collection loader is
        // itself released.
        MemoryFontFileLoader *memoryFileLoader_ = nullptr;
        MemoryFontCollectionLoader *memoryCollectionLoader_ = nullptr;

        friend class DWriteTextRect;
        friend class DWriteGlyphRun;
        friend class DWriteShaper;
        friend class DWriteFontFallback;

        // Phase 6 — text-layout-engine plumbing. One reusable shaper
        // instance (stateless modulo the analyzer it creates per call)
        // and one fallback driver keyed back to this engine. The
        // fallback driver maintains its own per-family substitute-Font
        // cache so repeated fallback for codepoints serviced by the
        // same face (e.g. every CJK ideograph routes to "MS Gothic")
        // returns one shared `Font` and one shared atlas.
        DWriteShaper shaper_;
        DWriteFontFallback fallback_{this};
    public:
        ITextShaper *   shaper()   override { return &shaper_;   }
        IFontFallback * fallback() override { return &fallback_; }
            DWriteFontEngineImpl(){
                  font_loader = std::make_shared<FontLoader>();
                    
                    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),(IUnknown **)&dwrite_factory);
                    if(FAILED(hr)){
                        exit(1);
                    };

                    // Phase 6: try to promote to IDWriteFactory2 for the
                    // system fallback driver. Pre-Win8.1 hosts return
                    // E_NOINTERFACE; in that case the fallback driver
                    // is inert and the layout engine leaves `.notdef`
                    // clusters as the requested face's missing-glyph
                    // box (same as a null fallback driver elsewhere).
                    // operator&() unwraps to the ComPtr's address so
                    // QueryInterface writes the ref into UniqueComPtr's
                    // backing pointer directly.
                    dwrite_factory->QueryInterface(IID_PPV_ARGS(&dwrite_factory2));

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

                    // Register the in-memory font loaders (asset-bundle
                    // font path). Both must outlive any font built from
                    // them: file loader holds the blob registry, the
                    // collection loader hands enumerators back to DWrite
                    // that build IDWriteFontFiles via the file loader.
                    memoryFileLoader_ = new MemoryFontFileLoader();
                    dwrite_factory->RegisterFontFileLoader(memoryFileLoader_);
                    memoryCollectionLoader_ = new MemoryFontCollectionLoader(memoryFileLoader_);
                    dwrite_factory->RegisterFontCollectionLoader(memoryCollectionLoader_);
            }
            // Fix for CreateFontA (So MSVC doesn't get confused with our function.)
            #ifdef CreateFont
            #undef CreateFont
            #endif
            
            Core::SharedPtr<Font> CreateFont(FontDescriptor & desc) override {
                        std::wstring w_str;
                        Native::cpp_str_to_cpp_wstr(desc.family,w_str);

                        DWRITE_FONT_WEIGHT weight;
                        DWRITE_FONT_STYLE style;
                        descToDWriteStyle(desc, weight, style);

                        // Resolve the system-collection face for the
                        // requested family + weight + style first. If
                        // the family isn't installed, fail loudly: a
                        // faceless DWriteFont silently lands in
                        // BitmapFallback mode and the MSDF path renders
                        // nothing, which surfaces to the user as a
                        // blank canvas with no clear cause. Returning
                        // nullptr lets the caller (test, app code) react
                        // — fall back to a different family, log, etc.
                        IDWriteFontCollection *systemColl = nullptr;
                        dwrite_factory->GetSystemFontCollection(&systemColl, FALSE);
                        IDWriteFontFace *face = resolveFontFace(systemColl, desc);
                        Core::SafeRelease(&systemColl);
                        if(face == nullptr){
                            if(textTraceEnabled()){
                                std::cout << "[wtk-text] CreateFont: '"
                                          << desc.family << "' size=" << desc.size
                                          << " not in system collection; returning nullptr"
                                          << std::endl;
                            }
                            return nullptr;
                        }

                        // Font size stays in DIPs. Physical pixel scaling is
                        // applied by the D2D device context via SetDpi() in
                        // DWriteTextRect, driven by ViewRenderTarget::renderScale.
                        /// TODO: Use Custom Fonts with custom font Collection!
                        IDWriteTextFormat *textFormat = nullptr;
                        HRESULT hr = dwrite_factory->CreateTextFormat(
                            w_str.c_str(), NULL, weight, style,
                            DWRITE_FONT_STRETCH_NORMAL, FLOAT(desc.size),
                            L"en-us", &textFormat);
                        if(FAILED(hr) || textFormat == nullptr){
                            if(textTraceEnabled()){
                                std::cout << "[wtk-text] CreateFont: '"
                                          << desc.family << "' size=" << desc.size
                                          << " CreateTextFormat failed hr=0x"
                                          << std::hex << hr << std::dec
                                          << "; returning nullptr" << std::endl;
                            }
                            Core::SafeRelease(&face);
                            return nullptr;
                        }

                        auto font = Core::SharedPtr<DWriteFont>(
                            new DWriteFont(desc, textFormat, face));

                        // Probe + install MSDF rasterize callback. Failure
                        // of any step leaves the font on BitmapFallback (the
                        // default installed by Font's base ctor) — that's
                        // still a valid `Font` (e.g. Apple Color Emoji-style
                        // bitmap-only faces), just not MSDF-routed.
                        probeAndInstallMsdf(*font);

                        return font;
            };
            Core::SharedPtr<Font> CreateFontFromFile(OmegaCommon::FS::Path path, FontDescriptor & desc)  override {
                auto path_ustring = OmegaCommon::UniString::fromUTF8(path.str().c_str());

                IDWriteFontCollection *collection = nullptr;
                HRESULT hr = dwrite_factory->CreateCustomFontCollection(
                    font_loader.get(),
                    path_ustring.getBuffer(),
                    path_ustring.length(),
                    &collection);
                if(FAILED(hr) || collection == nullptr){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] CreateFontFromFile: '" << path.str()
                                  << "' CreateCustomFontCollection failed hr=0x"
                                  << std::hex << hr << std::dec << "; returning nullptr"
                                  << std::endl;
                    }
                    return nullptr;
                }

                // Resolve face: first against the custom collection (the
                // loaded file), then fall back to the system collection
                // if the requested family doesn't match anything in the
                // file. If both miss, fail loudly — a faceless DWriteFont
                // silently lands in BitmapFallback and the MSDF path
                // renders nothing.
                IDWriteFontFace *face = resolveFontFace(collection, desc);
                if(face == nullptr){
                    IDWriteFontCollection *systemColl = nullptr;
                    dwrite_factory->GetSystemFontCollection(&systemColl, FALSE);
                    face = resolveFontFace(systemColl, desc);
                    Core::SafeRelease(&systemColl);
                }
                if(face == nullptr){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] CreateFontFromFile: '" << desc.family
                                  << "' size=" << desc.size
                                  << " not found in '" << path.str()
                                  << "' or system collection; returning nullptr"
                                  << std::endl;
                    }
                    Core::SafeRelease(&collection);
                    return nullptr;
                }

                // Build the text format against the custom collection so
                // the BitmapFallback path can draw the loaded face by
                // family name.
                std::wstring w_str;
                Native::cpp_str_to_cpp_wstr(desc.family, w_str);
                DWRITE_FONT_WEIGHT weight;
                DWRITE_FONT_STYLE style;
                descToDWriteStyle(desc, weight, style);
                IDWriteTextFormat *textFormat = nullptr;
                hr = dwrite_factory->CreateTextFormat(
                    w_str.c_str(), collection, weight, style,
                    DWRITE_FONT_STRETCH_NORMAL, FLOAT(desc.size),
                    L"en-us", &textFormat);
                Core::SafeRelease(&collection);
                if(FAILED(hr) || textFormat == nullptr){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] CreateFontFromFile: CreateTextFormat failed hr=0x"
                                  << std::hex << hr << std::dec << "; returning nullptr"
                                  << std::endl;
                    }
                    Core::SafeRelease(&face);
                    return nullptr;
                }

                auto font = Core::SharedPtr<DWriteFont>(
                    new DWriteFont(desc, textFormat, face));
                probeAndInstallMsdf(*font);
                return font;
            };

            /// Phase 6.7-c2: decide whether `font`'s resolved face
            /// exposes vector outlines that msdfgen can walk; if so,
            /// install a `RasterizeFn` on its atlas and promote it to
            /// `Mode::MSDF`. Otherwise log once (when traced) and leave
            /// it on `BitmapFallback`. Mirrors HarfBuzzFontEngine's
            /// `probeAndInstallMsdf` against DWrite.
            void probeAndInstallMsdf(DWriteFont &font) {
#ifdef OMEGAWTK_HAVE_MSDFGEN
                IDWriteFontFace *face = font.getFontFace();
                if(face == nullptr){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] DWriteFont: '"
                                  << font.desc.family << "' size=" << font.desc.size
                                  << " could not resolve IDWriteFontFace; using BitmapFallback"
                                  << std::endl;
                    }
                    return;
                }

                // Probe outline extractability against the 'A' glyph,
                // falling back to .notdef (glyph 0) if 'A' isn't mapped.
                // A face whose GetGlyphRunOutline succeeds but emits zero
                // segments is bitmap-only (`EBDT`/`EBLC` only) and must
                // stay on the BitmapFallback path.
                UINT32 probeCh = (UINT32)L'A';
                UINT16 probeGid = 0;
                face->GetGlyphIndices(&probeCh, 1, &probeGid);
                const UINT16 gidForProbe = probeGid; // 0 == .notdef is fine
                msdfgen::Shape probeShape;
                MsdfGeometrySink *probeSink = new MsdfGeometrySink(&probeShape);
                HRESULT probeHr = face->GetGlyphRunOutline(
                    FLOAT(font.desc.size),
                    &gidForProbe, nullptr, nullptr,
                    1,
                    FALSE, FALSE,
                    probeSink);
                const UINT32 probedSegments = probeSink->segmentCount();
                probeSink->Release();

                if(FAILED(probeHr) || probedSegments == 0){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] DWriteFont: '"
                                  << font.desc.family << "' size=" << font.desc.size
                                  << " outline probe hr=0x" << std::hex << probeHr
                                  << std::dec << " segments=" << probedSegments
                                  << "; using BitmapFallback" << std::endl;
                    }
                    return;
                }

                // Capture an extra ref on the face for the lambda. The
                // lambda is owned by the GlyphAtlas, whose lifetime is
                // tied to the WTK Font; DWriteFont's UniqueComPtr<Face>
                // is what holds the *other* ref. AddRef now, matched by
                // a Release in the shared-state destructor.
                struct LambdaState {
                    IDWriteFontFace *face;
                    unsigned size;
                    LambdaState(IDWriteFontFace *f, unsigned s) : face(f), size(s) {
                        if(face) face->AddRef();
                    }
                    ~LambdaState() { Core::SafeRelease(&face); }
                };
                auto state = std::make_shared<LambdaState>(face, font.desc.size);

                font.atlas().setRasterizeFn(
                    [state](std::uint32_t glyphId,
                            GlyphAtlas::RasterizedGlyph &out) -> bool {
                    if(state->face == nullptr){ return false; }

                    msdfgen::Shape shape;
                    MsdfGeometrySink *sink = new MsdfGeometrySink(&shape);
                    UINT16 gid = (UINT16)glyphId;
                    HRESULT hr = state->face->GetGlyphRunOutline(
                        FLOAT(state->size),
                        &gid, nullptr, nullptr,
                        1,
                        FALSE, FALSE,
                        sink);
                    sink->Release();
                    if(FAILED(hr)){
                        return false;
                    }

                    // msdfgen pipeline: normalize → orient contours →
                    // edge coloring → generate. `orientContours()`
                    // resolves the CW/CCW outer-contour-winding
                    // ambiguity between font formats and is essential
                    // for the signed-distance sign to come out right;
                    // skipping it inverts the fill on one whole family
                    // of fonts.
                    shape.normalize();
                    shape.orientContours();
                    msdfgen::edgeColoringSimple(shape, 3.0);

                    // Tight bbox + small padding, fit `kMsdfTileSize` to
                    // the larger dimension so atlas density matches the
                    // Linux path. Seed with `getBounds()` — `Shape::bound`
                    // *expands* the box, so a zero-seed would force the
                    // origin (0,0) into every bbox.
                    const msdfgen::Shape::Bounds bounds = shape.getBounds();
                    double l = bounds.l, b = bounds.b, r = bounds.r, t = bounds.t;
                    if(r <= l || t <= b){
                        return false;
                    }
                    const double padding = 2.0;
                    l -= padding; b -= padding; r += padding; t += padding;
                    const double scale = static_cast<double>(kMsdfTileSize) /
                                         std::max(r - l, t - b);
                    const unsigned tileW = std::max(1u,
                        static_cast<unsigned>(std::ceil((r - l) * scale)));
                    const unsigned tileH = std::max(1u,
                        static_cast<unsigned>(std::ceil((t - b) * scale)));
                    const msdfgen::Vector2 scaleV(scale, scale);
                    const msdfgen::Vector2 translate(-l, -b);

                    msdfgen::Bitmap<float, 3> msdf((int)tileW, (int)tileH);
                    msdfgen::generateMSDF(msdf, shape,
                                          msdfgen::Range(kMsdfRange / scale),
                                          scaleV, translate);

                    // Phase-2.5: reorient the msdfgen tile to Y-down so
                    // `GlyphAtlas` uploads straight (no row-flip) and
                    // `emitTextSubRun`'s canvas-top ↔ `v0` UV pairing
                    // carries the orientation through to the fragment
                    // with zero implicit flips. The geometry sink above
                    // already flipped DWrite's Y-down design space into
                    // a Y-up shape for msdfgen; this flips the
                    // generated tile back to row-0-is-top.
                    msdfgen::BitmapSection<float, 3> section = msdf;
                    section.reorient(msdfgen::Y_DOWNWARD);

                    // Quantize float → uint8, straight through. No extra
                    // bias — `generateMSDF` already maps signed distance
                    // to [0, 1] with the glyph edge at 0.5.
                    out.pxW = tileW;
                    out.pxH = tileH;
                    out.rgb.resize(static_cast<std::size_t>(tileW) * tileH * 3);
                    for(unsigned y = 0; y < tileH; ++y){
                        for(unsigned x = 0; x < tileW; ++x){
                            const float *px = section((int)x, (int)y);
                            const auto quant = [](float v) {
                                const float s = std::clamp(v * 255.f + 0.5f, 0.f, 255.f);
                                return static_cast<std::uint8_t>(s);
                            };
                            const std::size_t i = (static_cast<std::size_t>(y) * tileW + x) * 3;
                            out.rgb[i + 0] = quant(px[0]);
                            out.rgb[i + 1] = quant(px[1]);
                            out.rgb[i + 2] = quant(px[2]);
                        }
                    }

                    // Advance in canvas pixels. `GetDesignGlyphMetrics`
                    // reports design units; convert at the font's emSize
                    // by `(units * emSize) / unitsPerEm`, matching what
                    // `GetGlyphRunOutline(emSize, ...)` emitted into the
                    // outline above.
                    DWRITE_FONT_METRICS fontMetrics {};
                    state->face->GetMetrics(&fontMetrics);
                    DWRITE_GLYPH_METRICS gm {};
                    state->face->GetDesignGlyphMetrics(&gid, 1, &gm, FALSE);
                    const double upem = static_cast<double>(fontMetrics.designUnitsPerEm);
                    const double emSize = static_cast<double>(state->size);
                    const double duToPx = emSize / upem;
                    out.metrics.advance = static_cast<float>(gm.advanceWidth * duToPx);

                    // Phase-2.5 Skia-style top-anchored metrics.
                    // `l, b, r, t` are the padded bbox extents in shape
                    // coords (Y-up after the sink-side sign flip, pen
                    // origin at 0). Convert to top-anchored canvas-space:
                    //   fLeft   = bbox left = l (positive → right of pen).
                    //   fTop    = distance from baseline up to bbox top = t.
                    //   fWidth  = bbox width  in font-pixels = r - l.
                    //   fHeight = bbox height in font-pixels = t - b.
                    // No `scale` round-trip — `fWidth/fHeight` are the
                    // exact canvas-pixel dimensions of the quad.
                    out.metrics.fLeft   = static_cast<float>(l);
                    out.metrics.fTop    = static_cast<float>(t);
                    out.metrics.fWidth  = static_cast<float>(r - l);
                    out.metrics.fHeight = static_cast<float>(t - b);

                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] DUMP gid=" << glyphId
                                  << " l=" << l << " b=" << b
                                  << " r=" << r << " t=" << t
                                  << " scale=" << scale
                                  << " tile=" << tileW << "x" << tileH << std::endl;
                    }
                    return true;
                });
                font.setMode(Font::Mode::MSDF);

                if(textTraceEnabled()){
                    std::cout << "[wtk-text] DWriteFont: '"
                              << font.desc.family << "' size=" << font.desc.size
                              << " (probedSegments=" << probedSegments
                              << ") -> MSDF mode" << std::endl;
                }

                // Smoke-rasterize the probe glyph so any breakage in the
                // outline → msdfgen pipeline surfaces at Font
                // construction time, not on the first drawText call.
                if(gidForProbe != 0){
                    const bool ok = font.atlas().ensureGlyph(gidForProbe);
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] DWriteFont: smoke ensureGlyph(gid="
                                  << gidForProbe << ") -> "
                                  << (ok ? "ok" : "FAILED") << std::endl;
                    }
                }
#else
                (void)font;
                if(textTraceEnabled()){
                    std::cout << "[wtk-text] DWriteFont: built without OMEGAWTK_HAVE_MSDFGEN; using BitmapFallback"
                              << std::endl;
                }
#endif // OMEGAWTK_HAVE_MSDFGEN
            };
            Core::SharedPtr<Font> CreateFontFromAsset(
                    OmegaCommon::AssetBundle *bundle,
                    const OmegaCommon::String &assetName,
                    FontDescriptor &desc) override {
                if(bundle == nullptr || memoryFileLoader_ == nullptr ||
                   memoryCollectionLoader_ == nullptr){
                    return nullptr;
                }
                // Pull the font bytes out of the bundle. `load` returns a
                // Result<Vector<uint8_t>, String>; treat both !isOk and
                // an empty payload as a load failure.
                auto loadResult = bundle->load(assetName);
                if(!loadResult.isOk()){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] CreateFontFromAsset: '" << assetName
                                  << "' bundle->load failed: " << loadResult.error()
                                  << std::endl;
                    }
                    return nullptr;
                }
                auto blob = std::make_shared<std::vector<std::uint8_t>>(
                    std::move(loadResult.value()));
                if(blob->empty()){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] CreateFontFromAsset: '" << assetName
                                  << "' bundle entry is empty" << std::endl;
                    }
                    return nullptr;
                }

                // Hand the blob to the file loader's registry. The
                // returned key is what DWrite passes back through
                // CreateStreamFromKey to read the file.
                const std::uint64_t blobKey = memoryFileLoader_->registerBlob(blob);

                IDWriteFontCollection *collection = nullptr;
                HRESULT hr = dwrite_factory->CreateCustomFontCollection(
                    memoryCollectionLoader_,
                    &blobKey, sizeof(blobKey),
                    &collection);
                if(FAILED(hr) || collection == nullptr){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] CreateFontFromAsset: '" << assetName
                                  << "' CreateCustomFontCollection failed hr=0x"
                                  << std::hex << hr << std::dec << std::endl;
                    }
                    return nullptr;
                }

                // Resolve the requested family + style against the loaded
                // file. Asset-loaded fonts are not expected to fall back
                // to the system collection — if the file doesn't carry
                // the requested family, it's a packaging error and the
                // caller should hear about it via nullptr.
                IDWriteFontFace *face = resolveFontFace(collection, desc);
                if(face == nullptr){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] CreateFontFromAsset: '" << desc.family
                                  << "' size=" << desc.size
                                  << " not present in bundle asset '" << assetName
                                  << "'; returning nullptr" << std::endl;
                    }
                    Core::SafeRelease(&collection);
                    return nullptr;
                }

                std::wstring w_str;
                Native::cpp_str_to_cpp_wstr(desc.family, w_str);
                DWRITE_FONT_WEIGHT weight;
                DWRITE_FONT_STYLE style;
                descToDWriteStyle(desc, weight, style);
                IDWriteTextFormat *textFormat = nullptr;
                hr = dwrite_factory->CreateTextFormat(
                    w_str.c_str(), collection, weight, style,
                    DWRITE_FONT_STRETCH_NORMAL, FLOAT(desc.size),
                    L"en-us", &textFormat);
                Core::SafeRelease(&collection);
                if(FAILED(hr) || textFormat == nullptr){
                    if(textTraceEnabled()){
                        std::cout << "[wtk-text] CreateFontFromAsset: CreateTextFormat failed hr=0x"
                                  << std::hex << hr << std::dec << std::endl;
                    }
                    Core::SafeRelease(&face);
                    return nullptr;
                }

                auto font = Core::SharedPtr<DWriteFont>(
                    new DWriteFont(desc, textFormat, face));
                probeAndInstallMsdf(*font);
                if(textTraceEnabled()){
                    std::cout << "[wtk-text] CreateFontFromAsset: '" << desc.family
                              << "' size=" << desc.size
                              << " loaded from bundle asset '" << assetName
                              << "' (" << blob->size() << " bytes) -> "
                              << (font->mode() == Font::Mode::MSDF ? "MSDF" : "BitmapFallback")
                              << std::endl;
                }
                return font;
            }

            // Phase 6: no `adoptResolvedFace` override on DWrite — the
            // base default returns nullptr. The shaper-side c4 adoption
            // path that Linux uses (`HarfBuzzGlyphRun::shape` →
            // `FontEngine::inst()->adoptResolvedFace`) has no analogue
            // here: DWrite's `IDWriteTextLayout` is never driven on the
            // MSDF path, and the legacy bitmap path's `DWriteGlyphRun::
            // shape()` routes any fallback to the bitmap renderer
            // wholesale rather than adopting per-substitute faces.
            // Fallback for the WTK layout-engine path goes through
            // `DWriteFontFallback::fallbackForCodepoint` (which calls
            // `CreateFont` to materialize a fresh `DWriteFont` with
            // its own text format + face), mirroring CoreText.

            ~DWriteFontEngineImpl(){
                dwrite_factory->UnregisterFontCollectionLoader(font_loader.get());
                font_loader.reset();
                if(memoryCollectionLoader_ != nullptr){
                    dwrite_factory->UnregisterFontCollectionLoader(memoryCollectionLoader_);
                    memoryCollectionLoader_->Release();
                    memoryCollectionLoader_ = nullptr;
                }
                if(memoryFileLoader_ != nullptr){
                    dwrite_factory->UnregisterFontFileLoader(memoryFileLoader_);
                    memoryFileLoader_->Release();
                    memoryFileLoader_ = nullptr;
                }
            }
        };


        // Phase 6 — out-of-line because shapeRun needs
        // DWriteFontEngineImpl's complete type to reach the
        // IDWriteFactory for analyzer creation.
        OmegaCommon::Vector<ShaperGlyph> DWriteShaper::shapeRun(const ShaperInput & input) {
            OmegaCommon::Vector<ShaperGlyph> out;
            if(input.font == nullptr || input.text.length() == 0){
                return out;
            }
            auto font = std::dynamic_pointer_cast<DWriteFont>(input.font);
            if(font == nullptr){
                return out;
            }
            IDWriteFontFace *face = font->getFontFace();
            if(face == nullptr){
                return out;
            }
            const WCHAR *text = reinterpret_cast<const WCHAR *>(input.text.getBuffer());
            const UINT32 textLen = (UINT32)input.text.length();
            if(text == nullptr || textLen == 0){
                return out;
            }
            auto *engine = static_cast<DWriteFontEngineImpl *>(FontEngine::inst());
            if(engine == nullptr || engine->dwrite_factory.comPtr.Get() == nullptr){
                return out;
            }

            IDWriteTextAnalyzer *analyzer = nullptr;
            if(FAILED(engine->dwrite_factory->CreateTextAnalyzer(&analyzer)) ||
               analyzer == nullptr){
                return out;
            }

            // Script analysis. The layout engine pre-segments by script
            // (Phase 3), but DWrite's shaper still needs a
            // DWRITE_SCRIPT_ANALYSIS to feed GetGlyphs — AnalyzeScript
            // gives us that for the single run we're shaping. For pure
            // Latin input we expect exactly one returned range; if
            // DWrite splits further we shape each sub-range and
            // concatenate (visual order = logical order for LTR; the
            // ranges arrive in logical order either way).
            auto *source = new DWriteAnalysisSource(text, textLen, L"en-us");
            auto *sink = new DWriteAnalysisSink();
            HRESULT hr = analyzer->AnalyzeScript(source, 0, textLen, sink);
            if(FAILED(hr) || sink->scriptRanges.empty()){
                sink->Release();
                source->Release();
                Core::SafeRelease(&analyzer);
                return out;
            }

            const double emSize = static_cast<double>(font->desc.size);
            const BOOL rtl = input.rightToLeft ? TRUE : FALSE;

            for(const auto &range : sink->scriptRanges){
                if(range.textLength == 0){ continue; }
                const WCHAR *rangeText = text + range.textPosition;
                const UINT32 rangeLen = range.textLength;

                // Spec-recommended sizing: 3 * len/2 + 16. Retry with
                // doubled buffer on E_NOT_SUFFICIENT_BUFFER.
                UINT32 maxGlyphCount = 3 * rangeLen / 2 + 16;
                std::vector<UINT16> clusterMap(rangeLen);
                std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> textProps(rangeLen);
                std::vector<UINT16> glyphIndices(maxGlyphCount);
                std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProps(maxGlyphCount);
                UINT32 actualGlyphCount = 0;

                hr = E_NOT_SUFFICIENT_BUFFER;
                for(int attempt = 0; attempt < 4 && hr == E_NOT_SUFFICIENT_BUFFER; ++attempt){
                    hr = analyzer->GetGlyphs(
                        rangeText, rangeLen,
                        face, FALSE /*isSideways*/, rtl,
                        &range.analysis,
                        L"en-us",
                        nullptr /*numberSubstitution*/,
                        nullptr /*features*/,
                        nullptr /*featureRangeLengths*/,
                        0 /*featureRanges*/,
                        maxGlyphCount,
                        clusterMap.data(),
                        textProps.data(),
                        glyphIndices.data(),
                        glyphProps.data(),
                        &actualGlyphCount);
                    if(hr == E_NOT_SUFFICIENT_BUFFER){
                        maxGlyphCount *= 2;
                        glyphIndices.assign(maxGlyphCount, 0);
                        glyphProps.assign(maxGlyphCount,
                                          DWRITE_SHAPING_GLYPH_PROPERTIES{});
                    }
                }
                if(FAILED(hr)){
                    continue;
                }

                std::vector<FLOAT> glyphAdvances(actualGlyphCount);
                std::vector<DWRITE_GLYPH_OFFSET> glyphOffsets(actualGlyphCount);
                hr = analyzer->GetGlyphPlacements(
                    rangeText, clusterMap.data(), textProps.data(), rangeLen,
                    glyphIndices.data(), glyphProps.data(), actualGlyphCount,
                    face, FLOAT(emSize),
                    FALSE, rtl,
                    &range.analysis,
                    L"en-us",
                    nullptr, nullptr, 0,
                    glyphAdvances.data(), glyphOffsets.data());
                if(FAILED(hr)){
                    continue;
                }

                // Invert clusterMap[] (text-index → first-glyph-index)
                // into a glyph-index → cluster (text-index) lookup so
                // we can populate `ShaperGlyph::cluster`. The wrap pass
                // expects HarfBuzz-style cluster semantics: non-strict-
                // monotonic per-glyph indices into the *original* shape
                // input string. Offset by `range.textPosition` so the
                // cluster index is relative to the full
                // `ShaperInput::text`, not the script range.
                //
                // For every glyph in [clusterMap[ci], clusterMap[ci+1])
                // the cluster is `ci` (or `ci + range.textPosition` in
                // input-relative coords).
                std::vector<std::int32_t> glyphCluster(actualGlyphCount, 0);
                if(actualGlyphCount > 0){
                    for(UINT32 ci = 0; ci < rangeLen; ++ci){
                        UINT32 gStart = clusterMap[ci];
                        UINT32 gEnd = (ci + 1 < rangeLen) ? clusterMap[ci + 1]
                                                          : actualGlyphCount;
                        for(UINT32 gi = gStart; gi < gEnd && gi < actualGlyphCount; ++gi){
                            glyphCluster[gi] = static_cast<std::int32_t>(
                                ci + range.textPosition);
                        }
                    }
                }

                out.reserve(out.size() + actualGlyphCount);
                for(UINT32 i = 0; i < actualGlyphCount; ++i){
                    ShaperGlyph g;
                    g.glyphId = (std::uint32_t)glyphIndices[i];
                    g.advance = static_cast<float>(glyphAdvances[i]);
                    g.xOffset = static_cast<float>(glyphOffsets[i].advanceOffset);
                    // DWRITE_GLYPH_OFFSET::ascenderOffset is positive
                    // toward the ascender (up). Our layout engine
                    // convention is Y-down baseline distance, so flip
                    // the sign — a positive HB-style yOffset moves the
                    // glyph upward (toward smaller canvas Y).
                    g.yOffset = -static_cast<float>(glyphOffsets[i].ascenderOffset);
                    g.cluster = glyphCluster[i];
                    out.push_back(g);
                }
            }

            sink->Release();
            source->Release();
            Core::SafeRelease(&analyzer);
            return out;
        }

        // Phase 6 — out-of-line because the cache-miss path delegates
        // back to `DWriteFontEngineImpl::CreateFont`, which has to be
        // complete before we can call it.
        Core::SharedPtr<Font> DWriteFontFallback::fallbackForCodepoint(
                Core::SharedPtr<Font> requested,
                std::uint32_t codepoint){
            if(engine_ == nullptr || requested == nullptr){
                return nullptr;
            }
            IDWriteFactory2 *f2 = engine_->dwrite_factory2.comPtr.Get();
            if(f2 == nullptr){
                // Pre-Win8.1 host: no IDWriteFontFallback available.
                // Leave `.notdef` clusters to render as the requested
                // face's missing-glyph box.
                return nullptr;
            }

            IDWriteFontFallback *fb = nullptr;
            if(FAILED(f2->GetSystemFontFallback(&fb)) || fb == nullptr){
                return nullptr;
            }

            // MapCharacters needs the requested face's family + weight
            // + style so it can prefer a substitute that matches the
            // requested style. Build a one-codepoint UTF-16 buffer
            // (surrogate pair for supplementary plane).
            WCHAR utf16[2] = {0, 0};
            UINT32 utf16Len = 0;
            if(codepoint < 0x10000u){
                utf16[0] = (WCHAR)codepoint;
                utf16Len = 1;
            } else if(codepoint <= 0x10FFFFu){
                const std::uint32_t v = codepoint - 0x10000u;
                utf16[0] = (WCHAR)(0xD800u + (v >> 10));
                utf16[1] = (WCHAR)(0xDC00u + (v & 0x3FFu));
                utf16Len = 2;
            } else {
                fb->Release();
                return nullptr;
            }

            auto *source = new DWriteAnalysisSource(utf16, utf16Len, L"en-us");

            std::wstring baseFamilyW;
            Native::cpp_str_to_cpp_wstr(requested->desc.family, baseFamilyW);
            DWRITE_FONT_WEIGHT reqWeight;
            DWRITE_FONT_STYLE reqStyle;
            descToDWriteStyle(requested->desc, reqWeight, reqStyle);

            UINT32 mappedLength = 0;
            IDWriteFont *mappedFont = nullptr;
            FLOAT mappedScale = 1.f;
            HRESULT hr = fb->MapCharacters(
                source,
                0, utf16Len,
                /*baseFontCollection*/ nullptr,
                baseFamilyW.c_str(),
                reqWeight,
                reqStyle,
                DWRITE_FONT_STRETCH_NORMAL,
                &mappedLength,
                &mappedFont,
                &mappedScale);
            source->Release();
            fb->Release();
            if(FAILED(hr) || mappedFont == nullptr || mappedLength == 0){
                if(mappedFont != nullptr) mappedFont->Release();
                return nullptr;
            }

            // Resolve the mapped IDWriteFont's family name so we can
            // (a) cache by it and (b) hand a FontDescriptor to
            // CreateFont. Mirrors CoreText's family-name cache key.
            IDWriteFontFamily *family = nullptr;
            hr = mappedFont->GetFontFamily(&family);
            mappedFont->Release();
            if(FAILED(hr) || family == nullptr){
                return nullptr;
            }
            IDWriteLocalizedStrings *familyNames = nullptr;
            hr = family->GetFamilyNames(&familyNames);
            Core::SafeRelease(&family);
            if(FAILED(hr) || familyNames == nullptr){
                return nullptr;
            }
            UINT32 nameIdx = 0;
            BOOL exists = FALSE;
            // Prefer "en-us"; fall back to index 0 if absent.
            if(FAILED(familyNames->FindLocaleName(L"en-us", &nameIdx, &exists)) ||
               !exists){
                nameIdx = 0;
            }
            UINT32 nameLen = 0;
            familyNames->GetStringLength(nameIdx, &nameLen);
            std::wstring familyW(nameLen + 1, L'\0');
            familyNames->GetString(nameIdx, &familyW[0], nameLen + 1);
            familyW.resize(nameLen);
            Core::SafeRelease(&familyNames);

            // Convert to UTF-8 for the FontDescriptor + cache key.
            std::string familyU8;
            if(nameLen > 0){
                const int needed = WideCharToMultiByte(CP_UTF8, 0,
                    familyW.c_str(), (int)nameLen,
                    nullptr, 0, nullptr, nullptr);
                if(needed > 0){
                    familyU8.resize((size_t)needed);
                    WideCharToMultiByte(CP_UTF8, 0,
                        familyW.c_str(), (int)nameLen,
                        &familyU8[0], needed, nullptr, nullptr);
                }
            }
            if(familyU8.empty()){
                return nullptr;
            }

            // Same-as-requested → MapCharacters declined to substitute;
            // the requested face does cover the codepoint (shaper
            // returned .notdef for some other reason). Don't loop.
            if(familyU8 == requested->desc.family){
                return nullptr;
            }

            auto it = byFamily_.find(familyU8);
            if(it != byFamily_.end()){
                return it->second;
            }

            // Materialize through CreateFont — goes through the MSDF
            // probe and produces a real DWriteFont with text format +
            // face + atlas wired up.
            FontDescriptor fbDesc(familyU8, requested->desc.size,
                                  requested->desc.style);
            Core::SharedPtr<Font> resolved = engine_->CreateFont(fbDesc);
            byFamily_[familyU8] = resolved;

            if(textTraceEnabled()){
                std::cout << "[wtk-text] DWriteFontFallback: cp=U+"
                          << std::hex << codepoint << std::dec
                          << " -> '" << familyU8 << "' mode="
                          << (resolved != nullptr
                              && resolved->mode() == Font::Mode::MSDF
                              ? "MSDF" : "BitmapFallback") << std::endl;
            }
            return resolved;
        }

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
         /// Hold onto the original string + font so `shape()` can drive
         /// `IDWriteTextAnalyzer` directly. The textLayout above stays
         /// for the BitmapFallback path (DWriteTextRect::drawRun).
         OmegaCommon::UniString str;
         Core::SharedPtr<DWriteFont> dwFont;

         explicit DWriteGlyphRun(const OmegaCommon::UniString & str, Core::SharedPtr<Font> &font)
             :str(str), dwFont(std::dynamic_pointer_cast<DWriteFont>(font)){
             auto *_font = (DWriteFont *)font.get();
             auto FontEngineImpl = (DWriteFontEngineImpl *)FontEngine::inst();
            FontEngineImpl->dwrite_factory->CreateTextLayout((WCHAR *)str.getBuffer(),str.length(),_font->textFormat.get(),0,0,&textLayout);
         }
         Composition::Rect getBoundingRectOfGlyphAtIndex(size_t glyphIdx) override {
            return Composition::Rect {{0.f,0.f},0,0};
         }

         // Phase 6.7-c3: shape via IDWriteTextAnalyzer::GetGlyphs +
         // GetGlyphPlacements. Single-line, no wrap, no line limit:
         // multi-line / wrap belongs to the upcoming text-layout-engine
         // plan. Fallback detection: any cluster mapping to glyph 0
         // (.notdef) flips `requiresFallback` and the caller routes the
         // whole string to the bitmap path, matching the c3 contract on
         // Linux (one sub-run per TextRun until c4 lands multi-atlas
         // adoption). DPR is applied downstream by the render viewport.
         GlyphRun::ShapedTextRun shape(const Composition::Rect &rect,
                                       const TextLayoutDescriptor &layoutDesc) override {
             GlyphRun::ShapedTextRun result;
             if(dwFont == nullptr){
                 result.requiresFallback = true;
                 return result;
             }
             IDWriteFontFace *face = dwFont->getFontFace();
             if(face == nullptr){
                 result.requiresFallback = true;
                 return result;
             }
             const WCHAR *text = reinterpret_cast<const WCHAR *>(str.getBuffer());
             const UINT32 textLen = (UINT32)str.length();
             if(text == nullptr || textLen == 0){
                 return result;
             }

             auto *FontEngineImpl = (DWriteFontEngineImpl *)FontEngine::inst();
             IDWriteTextAnalyzer *analyzer = nullptr;
             if(FAILED(FontEngineImpl->dwrite_factory->CreateTextAnalyzer(&analyzer)) ||
                analyzer == nullptr){
                 result.requiresFallback = true;
                 return result;
             }

             auto *source = new DWriteAnalysisSource(text, textLen, L"en-us");
             auto *sink = new DWriteAnalysisSink();
             // AnalyzeScript partitions the text into script ranges.
             // For pure Latin we expect a single range; mixed-script
             // strings (e.g. Latin + CJK) produce multiple ranges, and
             // any range whose glyphs resolve to .notdef against the
             // requested face will trip the fallback path below.
             HRESULT hr = analyzer->AnalyzeScript(source, 0, textLen, sink);
             if(FAILED(hr)){
                 sink->Release();
                 source->Release();
                 Core::SafeRelease(&analyzer);
                 result.requiresFallback = true;
                 return result;
             }

             // Font metric pixels — used for baseline placement and the
             // vertical-alignment offset. emSize = desc.size DIPs;
             // ascent/descent/lineGap in design units → pixels.
             DWRITE_FONT_METRICS fontMetrics {};
             face->GetMetrics(&fontMetrics);
             const double upem = static_cast<double>(fontMetrics.designUnitsPerEm);
             const double emSize = static_cast<double>(dwFont->desc.size);
             const double ascentPx  = fontMetrics.ascent  * emSize / upem;
             const double descentPx = fontMetrics.descent * emSize / upem;
             const double lineGapPx = fontMetrics.lineGap * emSize / upem;
             const double lineHeightPx = ascentPx + descentPx + lineGapPx;

             // Total width we'll measure as we shape (single line — see
             // function-doc comment); needed for horizontal alignment.
             // We assemble glyphs in two passes: first shape every
             // script range and stash glyph IDs / advances / offsets,
             // then position them along a single baseline.
             struct ShapedGlyph {
                 std::uint32_t glyphId;
                 float advance;
                 float offsetX;
                 float offsetY;
             };
             std::vector<ShapedGlyph> shaped;
             shaped.reserve(textLen + 16);

             bool fallback = false;

             for(const auto &range : sink->scriptRanges){
                 if(range.textLength == 0){ continue; }
                 const WCHAR *rangeText = text + range.textPosition;
                 const UINT32 rangeLen = range.textLength;

                 // Spec-recommended sizing: maxGlyphCount = 3 * len/2 +
                 // 16. If GetGlyphs returns E_NOT_SUFFICIENT_BUFFER we
                 // double and retry, capping at a sane upper bound.
                 UINT32 maxGlyphCount = 3 * rangeLen / 2 + 16;
                 std::vector<UINT16> clusterMap(rangeLen);
                 std::vector<DWRITE_SHAPING_TEXT_PROPERTIES> textProps(rangeLen);
                 std::vector<UINT16> glyphIndices(maxGlyphCount);
                 std::vector<DWRITE_SHAPING_GLYPH_PROPERTIES> glyphProps(maxGlyphCount);
                 UINT32 actualGlyphCount = 0;

                 hr = E_NOT_SUFFICIENT_BUFFER;
                 for(int attempt = 0; attempt < 4 && hr == E_NOT_SUFFICIENT_BUFFER; ++attempt){
                     hr = analyzer->GetGlyphs(
                         rangeText, rangeLen,
                         face, FALSE /*isSideways*/, FALSE /*isRightToLeft*/,
                         &range.analysis,
                         L"en-us",
                         nullptr /*numberSubstitution*/,
                         nullptr /*features*/,
                         nullptr /*featureRangeLengths*/,
                         0 /*featureRanges*/,
                         maxGlyphCount,
                         clusterMap.data(),
                         textProps.data(),
                         glyphIndices.data(),
                         glyphProps.data(),
                         &actualGlyphCount);
                     if(hr == E_NOT_SUFFICIENT_BUFFER){
                         maxGlyphCount *= 2;
                         glyphIndices.assign(maxGlyphCount, 0);
                         glyphProps.assign(maxGlyphCount, DWRITE_SHAPING_GLYPH_PROPERTIES{});
                     }
                 }
                 if(FAILED(hr)){
                     fallback = true;
                     break;
                 }

                 std::vector<FLOAT> glyphAdvances(actualGlyphCount);
                 std::vector<DWRITE_GLYPH_OFFSET> glyphOffsets(actualGlyphCount);
                 hr = analyzer->GetGlyphPlacements(
                     rangeText, clusterMap.data(), textProps.data(), rangeLen,
                     glyphIndices.data(), glyphProps.data(), actualGlyphCount,
                     face, FLOAT(emSize),
                     FALSE, FALSE,
                     &range.analysis,
                     L"en-us",
                     nullptr, nullptr, 0,
                     glyphAdvances.data(), glyphOffsets.data());
                 if(FAILED(hr)){
                     fallback = true;
                     break;
                 }

                 for(UINT32 i = 0; i < actualGlyphCount; ++i){
                     if(glyphIndices[i] == 0){
                         // Any .notdef cluster means the requested face
                         // doesn't cover this character — c3 bails to
                         // the bitmap path for the whole string.
                         fallback = true;
                         break;
                     }
                     ShapedGlyph g;
                     g.glyphId = (std::uint32_t)glyphIndices[i];
                     g.advance = glyphAdvances[i];
                     g.offsetX = glyphOffsets[i].advanceOffset;
                     g.offsetY = glyphOffsets[i].ascenderOffset;
                     shaped.push_back(g);
                 }
                 if(fallback){ break; }
             }

             sink->Release();
             source->Release();
             Core::SafeRelease(&analyzer);

             if(fallback){
                 result.requiresFallback = true;
                 return result;
             }

             // Total advance for horizontal alignment.
             double totalAdvance = 0.0;
             for(const auto &g : shaped){
                 totalAdvance += g.advance;
             }

             // Horizontal start X based on layoutDesc.alignment.
             double startX = 0.0;
             switch(layoutDesc.alignment){
                 case TextLayoutDescriptor::MiddleUpper:
                 case TextLayoutDescriptor::MiddleCenter:
                 case TextLayoutDescriptor::MiddleLower:
                     startX = (rect.w - totalAdvance) / 2.0;
                     break;
                 case TextLayoutDescriptor::RightUpper:
                 case TextLayoutDescriptor::RightCenter:
                 case TextLayoutDescriptor::RightLower:
                     startX = rect.w - totalAdvance;
                     break;
                 default:
                     startX = 0.0;
                     break;
             }
             if(startX < 0.0) startX = 0.0;

             // Vertical baseline placement. Upper: baseline = ascent
             // (top of glyph aligns to top of rect). Center: shift by
             // (rect.h - lineHeight)/2. Lower: baseline = rect.h -
             // descent. Mirrors the offset math the Linux backend
             // applied via pango_layout_get_pixel_size, but computed
             // from font metrics rather than a laid-out line because
             // we're shaping directly without a layout.
             double baselineY = ascentPx;
             const int vAlign = verticalAlignmentCategory(layoutDesc.alignment);
             if(vAlign == 1){
                 const double extra = (double)rect.h - lineHeightPx;
                 if(extra > 0.0){
                     baselineY = ascentPx + extra / 2.0;
                 }
             } else if(vAlign == 2){
                 const double extra = (double)rect.h - lineHeightPx;
                 if(extra > 0.0){
                     baselineY = ascentPx + extra;
                 }
             }

             // Emit positioned glyph pen origins. DWrite's glyph offset
             // `ascenderOffset` is positive *toward the ascender* (i.e.
             // up in DIP-space), so we subtract it from the canvas-space
             // (Y-down) baseline to push the glyph up. Atlas tile
             // placement on the render side uses `tileOrigin*` /
             // `tileScale` from the rasterize step.
             //
             // Chunk 4: accumulate into a single sub-run (DWriteFont as
             // primary). When this backend lights up multi-atlas
             // fallback, drive `IDWriteFontFallback::MapCharacters` and
             // group by resolved IDWriteFontFace, calling
             // `FontEngine::adoptResolvedFace` per substitute face.
             TextSubRun primarySubRun;
             primarySubRun.resolvedFont = std::static_pointer_cast<Font>(dwFont);
             double penX = startX;
             primarySubRun.glyphIds.reserve(shaped.size());
             primarySubRun.positions.reserve(shaped.size());
             for(const auto &g : shaped){
                 const double gx = penX + (double)g.offsetX;
                 const double gy = baselineY - (double)g.offsetY;
                 primarySubRun.glyphIds.push_back(g.glyphId);
                 primarySubRun.positions.push_back(
                     Composition::Point2D{(float)gx, (float)gy});
                 penX += (double)g.advance;
             }
             if(!primarySubRun.glyphIds.empty()){
                 result.subRuns.push_back(std::move(primarySubRun));
             }

             if(textTraceEnabled()){
                 std::size_t totalGlyphs = 0;
                 for(const auto &sr : result.subRuns) totalGlyphs += sr.glyphIds.size();
                 std::cout << "[wtk-text] DWriteGlyphRun::shape -> "
                           << (result.requiresFallback ? "FALLBACK" : "MSDF")
                           << ", subRuns=" << result.subRuns.size()
                           << ", glyphs=" << totalGlyphs << std::endl;
             }
             return result;
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
            //  auto run = std::dynamic_pointer_cast<DWriteGlyphRun>(glyphRun);
            // //  UINT dpi = GetDpiFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
            // //  FLOAT scaleFactor = FLOAT(dpi)/96.f;
            //  run->textLayout->SetMaxWidth(rect.w);
            //  run->textLayout->SetMaxHeight(rect.h);
            //  run->textLayout->SetParagraphAlignment(paraAlignment);
            //  run->textLayout->SetFlowDirection(flowDirection);
            //  run->textLayout->SetTextAlignment(textAlignment);
            //  run->textLayout->SetWordWrapping(wrapping);
            //  if(lineLimit > 0){
            //      UINT32 lineCount = 0;
            //      HRESULT metricsStatus = run->textLayout->GetLineMetrics(nullptr,0,&lineCount);
            //      if((metricsStatus == S_OK || metricsStatus == E_NOT_SUFFICIENT_BUFFER) && lineCount > 0){
            //          std::vector<DWRITE_LINE_METRICS> metrics(lineCount);
            //          if(SUCCEEDED(run->textLayout->GetLineMetrics(metrics.data(),lineCount,&lineCount))){
            //              const UINT32 cappedLineCount = (std::min)(lineLimit,lineCount);
            //              FLOAT maxHeight = 0.f;
            //              for(UINT32 idx = 0; idx < cappedLineCount; ++idx){
            //                  maxHeight += metrics[idx].height;
            //              }
            //              if(maxHeight > 0.f){
            //                  run->textLayout->SetMaxHeight(maxHeight);
            //              }
            //          }
            //      }
            //  }
            //    auto *FontEngineImpl = dynamic_cast<DWriteFontEngineImpl *>(FontEngine::inst());
            //    FontEngineImpl->d3d11_device->AcquireWrappedResources((ID3D11Resource *const *)&resource,1);

            //  context->BeginDraw();
            //  context->Clear(D2D1::ColorF(0.f,0.f,0.f,0.f));
            //  ID2D1Brush * brush = nullptr;
            //  context->CreateSolidColorBrush(D2D1::ColorF(color.r,color.g,color.b,color.a),(ID2D1SolidColorBrush **)&brush);
            //  context->DrawTextLayout(D2D1::Point2F(0,0),run->textLayout.get(),brush,D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
            //  HRESULT hr = context->EndDraw();

            //   FontEngineImpl->d3d11_device->ReleaseWrappedResources((ID3D11Resource *const *)&resource,1);

            //  FontEngineImpl->d3d11_devicecontext->Flush();

            // auto native_fence = (ID3D12Fence *)fence->native();

            // // Signal the fence from the D3D11On12 queue so we can block
            // // the CPU until the D2D rasterisation has fully committed to
            // // the wrapped D3D12 resource. Using a CPU wait rather than a
            // // cross-queue GEFence wait avoids two problems:
            // //   1. The external Signal bypasses GED3D12Fence's cached
            // //      lastSignaledValue, so GECommandQueue::notifyCommandBuffer
            // //      would silently skip its commandQueue->Wait.
            // //   2. Returning a null fence from toBitmap lets the compositor
            // //      path mirror Metal exactly (no endRenderPass churn).
            // FontEngineImpl->d3d11_on_12_queue->Signal(native_fence,1);
            // if(native_fence->GetCompletedValue() < 1){
            //     HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            //     if(ev != nullptr){
            //         native_fence->SetEventOnCompletion(1, ev);
            //         WaitForSingleObject(ev, INFINITE);
            //         CloseHandle(ev);
            //     }
            // }

            //  Core::SafeRelease(&brush);
            //  if(hr == D2DERR_RECREATE_TARGET){
            //      Core::SafeRelease(&context);
            //      D2D1CreateDeviceContext(surface,D2D1::CreationProperties(D2D1_THREADING_MODE_MULTI_THREADED,D2D1_DEBUG_LEVEL_WARNING,D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS),&context);
            //  }
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

            //  OmegaGTE::TextureDescriptor textureDesc {};
            //  textureDesc.usage = OmegaGTE::GETexture::RenderTarget;
            //  textureDesc.pixelFormat = OmegaGTE::PixelFormat::BGRA8Unorm;
            //  textureDesc.height = (unsigned)(rect.h * this->renderScale);
            //  textureDesc.width = (unsigned)(rect.w * this->renderScale);

            //  target = gte.graphicsEngine->makeTexture(textureDesc);
             
            //  D3D11_RESOURCE_FLAGS fgs{};
            //  fgs.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            //  fgs.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            //  fgs.CPUAccessFlags = D3D11_USAGE_DEFAULT;
            //  fgs.StructureByteStride = 0.f;

            //  OMEGAWTK_DEBUG("Ok! 1");

            //   auto FontEngineImpl = (DWriteFontEngineImpl *)FontEngine::inst();

            //  // OutState intentionally stays at RENDER_TARGET: that's the
            //  // state GED3D12Texture caches for a usage=RenderTarget texture
            //  // (see gte/src/d3d12/GED3D12.cpp makeTexture). If OutState were
            //  // PIXEL_SHADER_RESOURCE, 11on12 would transition the actual
            //  // D3D12 state but GED3D12Texture::currentState would stay
            //  // RENDER_TARGET, and the next bindResourceAtFragmentShader
            //  // would emit a malformed RENDER_TARGET→PIXEL_SHADER_RESOURCE
            //  // barrier against a resource already in PIXEL_SHADER_RESOURCE.
            //  // Leaving it in RENDER_TARGET lets the compositor's normal
            //  // bind path issue a correct transition barrier.
            //  hr = FontEngineImpl->d3d11_device->CreateWrappedResource((IUnknown *)target->native(),&fgs,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,IID_PPV_ARGS(&resource));
            //  if(FAILED(hr)){
            //      OMEGAWTK_DEBUG("Failed to Create Wrapped Resource. ERR:" << std::hex << hr << std::dec);
            //      exit(1);
            //  }

            //   OMEGAWTK_DEBUG("Ok! 2");
            //  hr = resource->QueryInterface(IID_PPV_ARGS(&surface));
            //  if(FAILED(hr)){
            //       OMEGAWTK_DEBUG("Failed to get DXGISurface. ERR:" << std::hex << hr << std::dec);
            //      exit(1);
            //  }

            //  OMEGAWTK_DEBUG("Ok! 3");
            //  hr = FontEngineImpl->d2d1device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_ENABLE_MULTITHREADED_OPTIMIZATIONS,&context);
            //  if(FAILED(hr)){
            //       OMEGAWTK_DEBUG("Failed to create D2D1DeviceContext ERR:" << std::hex << hr << std::dec);
            //      exit(1);
            //  }

            //  OMEGAWTK_DEBUG("Ok! 4");

            //  ID2D1Bitmap1 *bitmap;

            //  // Stamp the bitmap with 96 * renderScale DPI so its *logical*
            //  // (DIP) size equals rect.w × rect.h — matching the DIP-valued
            //  // text layout box and the DIP-valued font size. Without this
            //  // the bitmap defaults to 96 DPI, so its logical size becomes
            //  // rect.w*scale × rect.h*scale DIPs and DrawTextLayout with
            //  // MaxWidth=rect.w only fills the top-left corner.
            // hr = context->CreateBitmapFromDxgiSurface(surface,
            //     D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            //         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            //             D2D1_ALPHA_MODE_PREMULTIPLIED),
            //             96.f * this->renderScale,96.f * this->renderScale),&bitmap);
             
            // if(FAILED(hr)){
            //       OMEGAWTK_DEBUG("Failed to create Bitmap from DXGISurface ERR:" << std::hex << hr << std::dec);
            //      exit(1);
            //  }
            //  context->SetTarget(bitmap);

            //  OMEGAWTK_DEBUG("Ok! 5");

            //  context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            //  // No explicit transform: the bitmap is already authored at
            //  // 96*renderScale DPI, so draw calls in DIPs are rasterised
            //  // into physical pixels automatically. Adding a SetTransform
            //  // scale on top would double-scale.
            //  context->SetTransform(D2D1::Matrix3x2F::Identity());
            //   OMEGAWTK_DEBUG("DWriteTextRect Successfully Created");

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

