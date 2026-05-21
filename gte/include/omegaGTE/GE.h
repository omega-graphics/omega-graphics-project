#include "omegasl.h"

#include <initializer_list>
#include <utility>
#include <variant>

#if !defined(TARGET_METAL) & !defined(TARGET_DIRECTX) & !defined(TARGET_VULKAN)
#error "Cannot Compile/Link OmegaGTE without specifiying Target Platform"
#endif

#if defined(TARGET_DIRECTX)

#include <windows.h>
#include <dxgi1_6.h>

#include <sdkddkver.h>
// /// If Windows Version is Greater then Windows 10 1809 (Redstone 5)
// #if NTDDI_VERSION >= NTDDI_WIN10_RS5
// #define OMEGAGTE_RAYTRACING_SUPPORTED 1
// #endif

#define DEBUG_ENGINE_PREFIX "GED3D12Engine_Internal"
#endif

#if defined(TARGET_METAL)
#include <Availability.h>

#ifdef __OBJC__
@class CAMetalLayer;
#endif

#define DEBUG_ENGINE_PREFIX "GEMetalEngine_Internal"

// #if defined(__MAC_OS_X_VERSION_MIN_REQUIRED)
//     #if __MAC_OS_X_VERSION_MIN_REQUIRED >= __MAC_11_0  
//         #define OMEGAGTE_RAYTRACING_SUPPORTED 1
//     #endif
// #elif defined(__IPHONE_OS_VERSION_MIN_REQUIRED)
//     #if __IPHONE_OS_VERSION_MIN_REQUIRED >= __IPHONE_14_0 
//         #define OMEGAGTE_RAYTRACING_SUPPORTED 1
//     #endif
// #endif

#endif

#if defined(TARGET_VULKAN)

#ifdef VULKAN_TARGET_WAYLAND
#include <wayland-client.h>
#endif

#ifdef VULKAN_TARGET_X11
#include <X11/Xlib.h>
#endif

#ifdef VULKAN_TARGET_ANDROID
#include <android/native_window.h>
#endif

#define DEBUG_ENGINE_PREFIX "GEVulkanEngine_Internal"

// #define OMEGAGTE_RAYTRACING_SUPPORTED 1
#endif

#include "GTEBase.h"

#ifndef OMEGAGTE_GE_H
#define OMEGAGTE_GE_H

_NAMESPACE_BEGIN_

/// Whether the debug layer is active. Defined in OmegaGTE.cpp and
/// re-declared here so DEBUG_STREAM can gate without pulling OmegaGTE.h.
/// Resolved at @c Init() time from @c GTEInitOptions::debugLayer.
OMEGAGTE_EXPORT bool isDebugLayerEnabled();

_NAMESPACE_END_

#define DEBUG_STREAM(message)                                                  \
    do {                                                                       \
        if (::OmegaGTE::isDebugLayerEnabled()) {                               \
            std::cout << "[" << DEBUG_ENGINE_PREFIX << "] - " << message       \
                      << std::endl;                                            \
        }                                                                      \
    } while (0)

_NAMESPACE_BEGIN_


    struct GTE;
    typedef enum : uint8_t {
        Shared,
        GPUOnly
    } StorageOpts;

    struct GTEShader;
    struct GTEShaderLibrary;
    struct GTEDevice;
    struct GETextureRegion;
    class GETexture;
    typedef struct __GEComputePipelineState GEComputePipelineState;
    typedef struct __GERenderPipelineState  GERenderPipelineState;
    typedef struct __GEBlitPipelineState    GEBlitPipelineState;
    class GENativeRenderTarget;
    class GETextureRenderTarget;
    class GECommandQueue;

    struct TextureDescriptor;
    struct TextureRenderTargetDescriptor;
    struct RenderPipelineDescriptor;
    struct ComputePipelineDescriptor;
    struct BlitPipelineDescriptor;

    struct NativeRenderTargetDescriptor;

    /// @brief Describes a Texture Render Target
    struct TextureRenderTargetDescriptor {
        bool renderToExistingTexture = false;
        SharedHandle<GETexture> texture = nullptr;
        TextureRegion region;
    };

    /// @brief A 3D Space sized to fixed dimensions.
    struct OMEGAGTE_EXPORT GEViewport {
        float x,y;
        float width,height;
        float nearDepth,farDepth;
    };

    /// @brief A Cropping Rectangle that clips the GEViewport.
    struct OMEGAGTE_EXPORT GEScissorRect {
        float x,y;
        float width,height;
    };

    /// @brief Describes a Buffer.
    /// @paragraph Each object in the buffer MUST be the identical.
    struct  OMEGAGTE_EXPORT BufferDescriptor {
        /// Describes the usage of the Buffer.
        typedef enum : int {
            Upload,
            Readback,
            GPUOnly
        } Usage;
        /// @enum Usage
        Usage usage = Upload;
        /// The length of the buffer (in bytes).
        size_t len = 0;
        /// The stride of each object in the buffer (in bytes).
        size_t objectStride = 0;
        /// The storage options of the resource.
        StorageOpts opts = Shared;
        /// @brief Binding role of the buffer — how it is bound to a shader.
        /// @paragraph Distinct from @ref Usage (which is memory residency /
        /// CPU access). A `Storage` buffer maps to the shader's `buffer<T>`
        /// form (StructuredBuffer / SSBO / `device T*`); a `Uniform` buffer
        /// maps to the `uniform<T>` form (ConstantBuffer / UBO / `constant
        /// T&`). This must match the shader resource the buffer is bound to.
        /// It drives creation-time allocation — Vulkan adds
        /// `VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT`, D3D12 aligns/pads to the
        /// 256-byte constant-buffer requirement — so the binding (chosen
        /// from the shader's layout descriptor) finds a compatible resource.
        /// Declared last so existing positional aggregate initializers
        /// (`{usage, len, stride}`) keep compiling and default to `Storage`.
        typedef enum : int {
            Storage,
            Uniform
        } Role;
        /// @enum Role
        Role role = Storage;
    };
    /// @brief A GPU Buffer Resource
    class  OMEGAGTE_EXPORT GEBuffer : public GTEResource {
    protected:
        BufferDescriptor::Usage usage;
        bool checkCanWrite();
        bool checkCanRead();
        explicit GEBuffer(const BufferDescriptor::Usage & usage);
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GEBuffer");
        /// @brief Binding role this buffer was created with (see
        /// @ref BufferDescriptor::Role). Set by `makeBuffer`; consulted at
        /// bind time to verify a buffer reaches a slot of the matching kind.
        BufferDescriptor::Role role = BufferDescriptor::Storage;
        virtual size_t size() = 0;
        virtual ~GEBuffer() = default;
    };

    struct  OMEGAGTE_EXPORT HeapDescriptor {
    public:
        typedef enum : uint8_t {
            Shared,
            Automatic
        } HeapType;
        size_t len;
    };

    class  OMEGAGTE_EXPORT GEHeap {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GEHeap")
        virtual size_t currentSize() = 0;
        /**
         @brief Creates a GEBuffer from a BufferDescriptor.
         @param[in] desc The Buffer Descriptor, which could describe a buffer at any length with any object.
         @returns SharedHandle<GEBuffer>
        */
        virtual SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor & desc) = 0;

        /**
          @brief Creates a GETexture from a TextureDescriptor.
         @param[in] desc The Texture Descriptor,  which could describe a 2D, 3D, 2D-Multisampled,or 3D-Multisampled texture with any given width, height (and depth).
         @returns SharedHandle<GETexture>
        */
        virtual SharedHandle<GETexture> makeTexture(const TextureDescriptor & desc) = 0;
        virtual ~GEHeap() = default;
    };

    /// @brief Provides command synchronization across multiple command queues.
    class  OMEGAGTE_EXPORT GEFence : public GTEResource {
    public:
        OMEGACOMMON_CLASS("OmegaGTE.GEFence")
        /// Value last signaled (or about to be signaled) by the producer queue; use for CPU wait.
        virtual std::uint64_t getLastSignaledValue() const { return 0; }

        virtual ~GEFence() = default;
    };

    /// @brief Describes a Texture Sampler
    struct OMEGAGTE_EXPORT SamplerDescriptor {
        OmegaCommon::StrRef name;
        enum class AddressMode  : int {
            Wrap,
            ClampToEdge,
            MirrorClampToEdge,
            MirrorWrap,
        }
        /// @brief  Address Mode for Width
        uAddressMode = AddressMode::Wrap,
        /// @brief Address Mode for Height
        vAddressMode = AddressMode::Wrap,
        /// @brief Address Mode for Depth
        wAddressMode = AddressMode::Wrap;
        enum class Filter : int {
            Linear,
            Point,
            MagLinearMinPointMipLinear,
            MagPointMinLinearMipLinear,
            MagLinearMinLinearMipPoint,
            MagPointMinLinearMipPoint,
            MagLinearMinPointMipPoint,
            MagPointMinPointMipLinear,
            MaxAnisotropic,
            MinAnisotropic
        } filter = Filter::Linear;
        unsigned int maxAnisotropy = 16;

    };

    class OMEGAGTE_EXPORT GESamplerState { public: OMEGACOMMON_CLASS("OmegaGTE.GESamplerState")};


    struct GERaytracingBoundingBox {
        float minX,minY,minZ,maxX,maxY,maxZ;
    };

     /// @brief Describes the Layout of a Acceleration Structure.
    struct OMEGAGTE_EXPORT GEAccelerationStructDescriptor {
        struct Geometry {
            enum : int {
                TRIANGLES,
                AABB
            } type;
            struct TriangleList { SharedHandle<GEBuffer> buffer; };
            struct Aabb { SharedHandle<GEBuffer> buffer; };
            std::variant<TriangleList, Aabb> data;

            Geometry() : type(TRIANGLES), data(TriangleList{}) {}
            void setTriangleList(SharedHandle<GEBuffer>& buffer) { type = TRIANGLES; data = TriangleList{buffer}; }
            void setAabb(SharedHandle<GEBuffer>& buffer) { type = AABB; data = Aabb{buffer}; }
            TriangleList& getTriangleList() { return std::get<TriangleList>(data); }
            const TriangleList& getTriangleList() const { return std::get<TriangleList>(data); }
            Aabb& getAabb() { return std::get<Aabb>(data); }
            const Aabb& getAabb() const { return std::get<Aabb>(data); }
        };
        OmegaCommon::Vector<Geometry> data;
    public:
        void addTriangleBuffer(SharedHandle<GEBuffer> & buffer){
            Geometry g;
            g.setTriangleList(buffer);
            data.push_back(g);
        }
        void addBoundingBoxBuffer(SharedHandle<GEBuffer> & buffer){
            Geometry g;
            g.setAabb(buffer);
            data.push_back(g);
        }
    };

    struct OMEGAGTE_EXPORT GEAccelerationStruct {
        OMEGACOMMON_CLASS("OmegaGTE.GEAccelerationStruct");
        virtual ~GEAccelerationStruct() = default;
    };

    /**
     @brief The Omega Graphics Engine
    */
    class OMEGAGTE_EXPORT OmegaGraphicsEngine {
        SharedHandle<GTEShaderLibrary> loadShaderLibraryFromInputStream(std::istream & in);
    protected:
        virtual SharedHandle<GTEShader> _loadShaderFromDesc(omegasl_shader *shaderDesc,bool runtime = false) = 0;

        /**
          @brief Bitmask of @c OMEGASL_FEATURE_BIT_* flags the active device
          can satisfy. Each backend caches the result of
          @c GTEDeviceFeatures::featuresAsBitmask() into this field during
          construction. Consumed by @c loadShaderLibraryFromInputStream and
          @c loadShaderLibraryRuntime to mask each shader's @c requiredFeatures
          and reject only the shaders the device can't run; sibling shaders
          continue to load. See OmegaSL-Feature-Gap-Survey §14.3 / §14.7.1
          task B.
         */
        uint64_t _deviceFeatures = 0;

        /**
          @brief Format a human-readable rejection diagnostic.
          @paragraph Names the @c OMEGASL_FEATURE_BIT_* bits set in
          @p requiredFeatures and @p missingFeatures using the symbolic
          spellings from @c omegasl.h. Output shape:
          <tt>requires features [A, B]; device lacks [B]</tt>.
         */
        static std::string _formatMissingFeatures(uint64_t requiredFeatures,
                                                  uint64_t missingFeatures);

        /**
          @brief Construct a Layer-3 rejection sentinel for a shader the
          device cannot run. The sentinel is a base @c GTEShader (no
          backend-specific subclass) carrying @c isUnsupported = true and
          the supplied diagnostic. Pipeline builders detect the sentinel
          via @c _checkPipelineShader and surface @p diagnostic at
          pipeline-creation time. See OmegaSL-Feature-Gap-Survey §14.7.1
          tasks B and C.
         */
        static SharedHandle<GTEShader> _makeUnsupportedShaderSentinel(std::string diagnostic);

        /**
          @brief Validate a shader handle before pipeline construction.
          @paragraph Returns @c false and writes a precise message to
          @c stderr when @p shader is @c nullptr or carries @c isUnsupported.
          The @p role argument names the slot (@c "vertex", @c "fragment",
          @c "compute") and @p pipelineName is used to attribute the error.
          Returns @c true for ordinary shaders so pipeline builders can
          proceed.
         */
        static bool _checkPipelineShader(const SharedHandle<GTEShader> &shader,
                                         const char *role,
                                         const OmegaCommon::String &pipelineName);
    public:
        OMEGACOMMON_CLASS("OmegaGTE.OmegaGraphicsEngine")
        /// @brief Returns the Native Device.
        /// @returns On Windows, it returns ID3D12Device *, For Darwin, it returns a id<MTLDevice> ,and for Android and Linux it returns a VkDevice.
        virtual void * underlyingNativeDevice() = 0;
        /** 
        @brief Creates an Instance of the Omega Graphics Engine  
        (NEVER CALL THIS FUNCTION! Please invoke GTE::Init())
        @returns SharedHandle<OmegaGraphicsEngine>
        */
        static SharedHandle<OmegaGraphicsEngine> Create(SharedHandle<GTEDevice> & device);
         /**
          @brief Loads an OmegaSL Shader Library,
          @param path Path to an `omegasllib` file.
          @returns SharedHandle<GTEShaderLibrary>
         */
         SharedHandle<GTEShaderLibrary> loadShaderLibrary(FS::Path path);

        /**
         @brief Loads an OmegaSL Shader Library on Runtime.
         @param lib A std::shared_ptr to an a runtime compiled omegasl library.
         @returns SharedHandle<GTEShaderLibrary>
        */
        SharedHandle<GTEShaderLibrary> loadShaderLibraryRuntime(std::shared_ptr<omegasl_shader_lib> & lib);

        virtual SharedHandle<GEBuffer> createBoundingBoxesBuffer(OmegaCommon::ArrayRef<GERaytracingBoundingBox> boxes) = 0;

        /// @brief Allocate a Buffer to hold an Acceleration Structure.
        /// @returns SharedHandle<GEAccelerationStruct>
        virtual SharedHandle<GEAccelerationStruct> allocateAccelerationStructure(const GEAccelerationStructDescriptor & desc) = 0;
        /**
         @brief Creates a GEFence.
         @returns SharedHandle<GEFence>
         @see GEFence
        */
        virtual SharedHandle<GEFence> makeFence() = 0;

        /**
         @brief Creates a GESamplerState
         @returns SharedHandle<GESamplerState>
         * */
         virtual SharedHandle<GESamplerState> makeSamplerState(const SamplerDescriptor &desc) = 0;

        /**
         @brief Creates a GEBuffer from a BufferDescriptor.
         @param[in] desc The Buffer Descriptor, which could describe a buffer at any length with any object.
         @returns SharedHandle<GEBuffer>
        */
        virtual SharedHandle<GEBuffer> makeBuffer(const BufferDescriptor & desc) = 0;

        /**
         @brief Creates a GETexture from a TextureDescriptor.
         @param[in] desc The Texture Descriptor,  which could describe a 2D, 3D, 2D-Multisampled,or 3D-Multisampled texture with any given width, height (and depth).
         @returns SharedHandle<GETexture>
        */
        virtual SharedHandle<GETexture> makeTexture(const TextureDescriptor & desc) = 0;

        /**
         @brief Creates a GEHeap from a HeapDescriptor.
         @param[in] desc The Heap Descriptor
         @returns SharedHandle<GEHeap>
        */
        virtual SharedHandle<GEHeap> makeHeap(const HeapDescriptor & desc) = 0;
        

        /**
         @brief Creates a GERenderPipelineState from a RenderPipelineDescriptor.
         @param[in] desc The Render Pipeline Descriptor
         @returns SharedHandle<GERenderPipelineState>
        */
        virtual SharedHandle<GERenderPipelineState> makeRenderPipelineState(RenderPipelineDescriptor & desc) = 0;

        /**
         @brief Creates a GEComputePipelineState from a ComputePipelineDescriptor.
         @param[in] desc The Compute Pipeline State
         @returns SharedHandle<GEComputePipelineState>
        */
        virtual SharedHandle<GEComputePipelineState> makeComputePipelineState(ComputePipelineDescriptor & desc) = 0;

        /**
         @brief Creates a GEBlitPipelineState from a BlitPipelineDescriptor.
         @paragraph Builds a programmable blit pipeline: a render pipeline with
         an engine-supplied full-screen-triangle vertex shader paired with the
         caller's fragment shader. Used with @c GECommandBuffer::blitWithPipeline.
         @param[in] desc The Blit Pipeline Descriptor
         @returns SharedHandle<GEBlitPipelineState>, or @c nullptr on failure.
         */
        virtual SharedHandle<GEBlitPipelineState> makeBlitPipelineState(BlitPipelineDescriptor & desc) = 0;

        /**
          @brief Creates a GENativeRenderTarget from a NativeRenderTargetDescriptor.
          @param[in] desc The Native Render Target Descriptor
          @param[in] presentQueue The command queue the swap chain will be created
                     against and that `present()` will submit on. On D3D12 / Vulkan
                     the swap chain is bound to this queue at creation time and
                     cannot be re-targeted. On Metal the queue is recorded so the
                     engine can encode the `presentDrawable:` call.
          @returns SharedHandle<GENativeRenderTarget>
         */
        virtual SharedHandle<GENativeRenderTarget>
            makeNativeRenderTarget(const NativeRenderTargetDescriptor & desc,
                                    SharedHandle<GECommandQueue> presentQueue) = 0;

        /**
          @brief Creates a GETextureRenderTarget from a TextureRenderTargetDescriptor.
          Texture render targets do not own a queue — whichever queue the user
          submits draw/blit work on is responsible for the underlying texture.
          @param[in] desc The Texture Render Target Descriptor
          @returns SharedHandle<GETextureRenderTarget>
         */
        virtual SharedHandle<GETextureRenderTarget> makeTextureRenderTarget(const TextureRenderTargetDescriptor & desc) = 0;

        /**
          @brief Creates a GECommmandQueue with a maximum number of usable command buffers.
          @param[in] maxBufferCount The command buffers to allocate.
          @returns SharedHandle<GECommandQueue>
         */
        virtual SharedHandle<GECommandQueue> makeCommandQueue(unsigned maxBufferCount) = 0;
        /// TODO:
        /// Cleaner API for GE.h
        virtual void waitForGPUIdle() {}

        /// Diagnostic: read RGBA8 pixel from a texture's level-0 layer-0 at (x,y).
        /// Returns true on success and writes 4 bytes (R,G,B,A) to out.
        virtual bool debugReadbackPixelRGBA8(SharedHandle<GETexture>, unsigned, unsigned, std::uint8_t[4]) { return false; }

        virtual ~OmegaGraphicsEngine() = default;
    };




    struct OMEGAGTE_EXPORT NativeRenderTargetDescriptor {
        bool allowDepthStencilTesting = false;
        /// Color format for the swap chain / drawable. Only the portable
        /// intersection of D3D12, Metal, and Vulkan swap-chain formats is
        /// guaranteed to succeed across backends — see
        /// `isPortableNativeRenderTargetFormat`. Default `BGRA8Unorm` is the
        /// only format universally supported by all three (Metal's
        /// `CAMetalLayer` rejects RGBA8, D3D12 FLIP-model swap chains accept
        /// it, and it is the most commonly advertised surface format on
        /// Vulkan/Win32, Wayland, and X11).
        PixelFormat pixelFormat = PixelFormat::BGRA8Unorm;
#ifdef TARGET_DIRECTX
        bool isHwnd;
        HWND hwnd;
        unsigned width;
        unsigned height;
#endif

#if defined(TARGET_METAL) && defined(__OBJC__)
        CAMetalLayer *metalLayer;
#endif

#if defined(TARGET_VULKAN)
#ifdef VULKAN_TARGET_X11
        Window x_window;
        Display *x_display;
#endif
#ifdef VULKAN_TARGET_WAYLAND
        wl_surface *wl_surface = nullptr;
        wl_display *wl_display = nullptr;
        unsigned width;
        unsigned height;
#endif
#ifdef VULKAN_TARGET_ANDROID
        ANativeWindow *window;
#endif
#endif
    };

    /// @brief Returns true for `PixelFormat` values that are valid swap-chain
    /// / drawable formats on every backend OmegaGTE supports (D3D12, Metal,
    /// Vulkan). Use this to validate `NativeRenderTargetDescriptor::pixelFormat`
    /// at the public API boundary before handing off to a backend.
    ///
    /// The portable set is currently: `BGRA8Unorm`, `BGRA8Unorm_SRGB`.
    /// `RGBA8Unorm`/`RGBA8Unorm_SRGB` are excluded because `CAMetalLayer`
    /// does not accept them; `RGBA16Unorm` is excluded because no backend
    /// exposes a 16-bit-per-channel UNORM swap-chain format.
    OMEGAGTE_EXPORT bool isPortableNativeRenderTargetFormat(PixelFormat fmt);


_NAMESPACE_END_

#endif
