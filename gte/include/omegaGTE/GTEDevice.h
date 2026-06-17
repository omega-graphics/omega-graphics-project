#ifndef OMEGAGTE_GTEDEVICE_H
#define OMEGAGTE_GTEDEVICE_H

#include "GTEBase.h"
#include "GE.h"
#include "TE.h"
#include "omegasl.h"

#include <cstdint>

_NAMESPACE_BEGIN_

/**
  @brief Capability bitflags for GTEDeviceFeatures::flags.
  @paragraph Each flag represents a discrete GPU capability that OmegaGTE can
  consume. Absence of a flag means the feature is unsupported on the device or
  not yet wired up by its backend. Flags are packed into a single uint64_t so
  the struct stays trivially copyable.
 */
constexpr uint64_t GTEDEVICE_FEATURE_RAYTRACING                    = 1ULL << 0;
constexpr uint64_t GTEDEVICE_FEATURE_MESH_SHADER                   = 1ULL << 1;
constexpr uint64_t GTEDEVICE_FEATURE_VARIABLE_RATE_SHADING         = 1ULL << 2;
constexpr uint64_t GTEDEVICE_FEATURE_CONSERVATIVE_RASTERIZATION    = 1ULL << 3;
constexpr uint64_t GTEDEVICE_FEATURE_INDEPENDENT_BLEND             = 1ULL << 4;
constexpr uint64_t GTEDEVICE_FEATURE_DUAL_SOURCE_BLENDING          = 1ULL << 5;
constexpr uint64_t GTEDEVICE_FEATURE_DEPTH_CLAMP                   = 1ULL << 6;
constexpr uint64_t GTEDEVICE_FEATURE_DEPTH_BIAS_CLAMP              = 1ULL << 7;
constexpr uint64_t GTEDEVICE_FEATURE_FILL_MODE_NON_SOLID           = 1ULL << 8;
constexpr uint64_t GTEDEVICE_FEATURE_WIDE_LINES                    = 1ULL << 9;
constexpr uint64_t GTEDEVICE_FEATURE_SAMPLER_ANISOTROPY            = 1ULL << 10;
constexpr uint64_t GTEDEVICE_FEATURE_MULTI_DRAW_INDIRECT           = 1ULL << 11;
constexpr uint64_t GTEDEVICE_FEATURE_DRAW_INDIRECT_FIRST_INSTANCE  = 1ULL << 12;
constexpr uint64_t GTEDEVICE_FEATURE_GEOMETRY_SHADER               = 1ULL << 13;
constexpr uint64_t GTEDEVICE_FEATURE_TESSELLATION_SHADER           = 1ULL << 14;
constexpr uint64_t GTEDEVICE_FEATURE_SHADER_BARYCENTRIC            = 1ULL << 15;
constexpr uint64_t GTEDEVICE_FEATURE_DESCRIPTOR_INDEXING           = 1ULL << 16;
constexpr uint64_t GTEDEVICE_FEATURE_SHADER_FLOAT16                = 1ULL << 17;
constexpr uint64_t GTEDEVICE_FEATURE_SHADER_INT16                  = 1ULL << 18;
constexpr uint64_t GTEDEVICE_FEATURE_SHADER_FLOAT64                = 1ULL << 19;
constexpr uint64_t GTEDEVICE_FEATURE_SHADER_INT64                  = 1ULL << 20;
constexpr uint64_t GTEDEVICE_FEATURE_TIMESTAMP_QUERIES             = 1ULL << 21;
constexpr uint64_t GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_BC        = 1ULL << 22;
constexpr uint64_t GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_ETC2      = 1ULL << 23;
constexpr uint64_t GTEDEVICE_FEATURE_TEXTURE_COMPRESSION_ASTC      = 1ULL << 24;

/**
  @brief All the features that are supported on a GTEDevice.
 * */
struct GTEDeviceFeatures {
    /// @brief Highest shader model tier the device supports.
    enum class ShaderModel : uint8_t {
        SM_5_0 = 0,   ///< baseline (D3D FL 11_0 / Vulkan 1.0 / Metal Common1)
        SM_5_1,        ///< D3D FL 11_1 / unbounded descriptor arrays
        SM_6_0,        ///< wave intrinsics
        SM_6_4,        ///< 16-bit types in shaders
        SM_6_5,        ///< mesh shaders, DXR 1.1
        SM_6_6,        ///< 64-bit atomics, dynamic resources
        SM_6_7,        ///< work graphs
    };

    /// Bitmask of GTEDEVICE_FEATURE_* capability flags.
    uint64_t flags = 0;

    /// Highest shader model tier the device supports.
    ShaderModel shaderModel = ShaderModel::SM_5_0;

    /// Maximum MSAA sample count supported (1, 2, 4, 8, 16, or 32).
    uint8_t maxMSAASamples = 1;

    // ── Texture limits ──────────────────────────────────────────
    uint32_t maxTextureDimension2D = 0;
    uint32_t maxTextureDimension3D = 0;
    uint32_t maxTextureDimensionCube = 0;

    // ── Buffer / memory limits ──────────────────────────────────
    uint64_t maxBufferSize = 0;

    // ── Compute limits ──────────────────────────────────────────
    uint32_t maxComputeWorkGroupSizeX = 0;
    uint32_t maxComputeWorkGroupSizeY = 0;
    uint32_t maxComputeWorkGroupSizeZ = 0;
    uint32_t maxComputeWorkGroupInvocations = 0;
    uint32_t maxComputeSharedMemorySize = 0;

    // ── Sampler ─────────────────────────────────────────────────
    uint32_t maxSamplerAnisotropy = 0;

    /// Nanoseconds per GPU timestamp tick (0 if timestamps unsupported).
    float timestampPeriod = 0.0f;

    /// @brief Test whether all bits in @p featureMask are set.
    constexpr bool hasFeature(uint64_t featureMask) const {
        return (flags & featureMask) == featureMask;
    }

    /**
      @brief Synthesize the @c OMEGASL_FEATURE_BIT_* mask matching this
      device's capabilities. Consumed by the runtime shader-library loader
      (see OmegaSL-Feature-Gap-Survey §14.3) to mask each shader's
      @c requiredFeatures and reject only the shaders the device can't run.
      @paragraph Phase A — covers the bits already backed by
      @c GTEDEVICE_FEATURE_* flags or the shader-model tier. Bits without
      backing storage today (SUBPASS_INPUTS, SPEC_CONSTANTS, TEXTURECUBE_RW,
      TEXTURE2D_MS_WRITE) stay zero, so a shader that requires them is
      conservatively rejected until @c GTEDeviceFeatures gains the matching
      flags (Phase B per §14.4).
     */
    constexpr uint64_t featuresAsBitmask() const {
        uint64_t mask = OMEGASL_FEATURE_BIT_NONE;
        if (flags & GTEDEVICE_FEATURE_RAYTRACING)
            mask |= OMEGASL_FEATURE_BIT_RAYTRACING;
        if (flags & GTEDEVICE_FEATURE_MESH_SHADER)
            mask |= OMEGASL_FEATURE_BIT_MESH_SHADERS;
        if (flags & GTEDEVICE_FEATURE_GEOMETRY_SHADER)
            mask |= OMEGASL_FEATURE_BIT_GEOMETRY_SHADERS;
        if (flags & GTEDEVICE_FEATURE_TESSELLATION_SHADER)
            mask |= OMEGASL_FEATURE_BIT_TESSELLATION;
        if (shaderModel >= ShaderModel::SM_6_0)
            mask |= OMEGASL_FEATURE_BIT_SUBGROUP_OPS;
        if (flags & GTEDEVICE_FEATURE_DESCRIPTOR_INDEXING)
            mask |= OMEGASL_FEATURE_BIT_BINDLESS;
        if (flags & GTEDEVICE_FEATURE_SHADER_FLOAT16)
            mask |= OMEGASL_FEATURE_BIT_FLOAT16;
        if (flags & GTEDEVICE_FEATURE_SHADER_INT64)
            mask |= OMEGASL_FEATURE_BIT_INT64;
        if (flags & GTEDEVICE_FEATURE_VARIABLE_RATE_SHADING)
            mask |= OMEGASL_FEATURE_BIT_VARIABLE_RATE;
        if (flags & GTEDEVICE_FEATURE_SHADER_FLOAT64)
            mask |= OMEGASL_FEATURE_BIT_DOUBLE;
        return mask;
    }
};

/**
  @brief Per-device memory budget snapshot.
  @paragraph Returned by GTEDevice::queryMemoryBudget(). On unified-memory
  architectures (Apple Silicon, integrated GPUs) @c dedicatedVideoMemory is
  zero and @c unifiedMemory is @c true; callers should use system memory
  queries for budgeting in that case. Values are in bytes.
 */
struct GTEDeviceMemoryBudget {
    uint64_t dedicatedVideoMemory = 0;
    uint64_t availableVideoMemory = 0;
    bool     unifiedMemory = false;
};

/**
  @brief A hardware (or software) device that is capable of performing graphical/matrix calculations.
  @paragraph This class is an easy interface for selecting the most suitable device to create the GTE.
 */
struct GTEDevice {
    /// @brief Defines the type of GPU device.
    using Type = enum : int {
        Integrated,
        Discrete
    };
    /// @enum Type
    const Type type;
    /// The Device name.
    const OmegaCommon::String name;
    /// The Device's features.
    const GTEDeviceFeatures features;
protected:
    GTEDevice(Type type,const char *name,GTEDeviceFeatures & features):type(type),name(name),features(features){

    };
public:
     OMEGACOMMON_CLASS("OmegaGTE.GTEDevice")
    virtual const void *native() = 0;
    /// @brief Query current GPU memory budget. Default returns zeros; backends override.
    virtual GTEDeviceMemoryBudget queryMemoryBudget() { return {}; }
    virtual ~GTEDevice() = default;
};

/** @brief Enumerate all GPU devices on this system.
 * @return A Vector of all the existing GTEDevices on this system.
 * */
OMEGAGTE_EXPORT OmegaCommon::Vector<SharedHandle<GTEDevice>> enumerateDevices();

/// @brief The Graphics and Triangulation Engine!
struct OMEGAGTE_EXPORT GTE {
    SharedHandle<OmegaGraphicsEngine> graphicsEngine;
    SharedHandle<OmegaTriangulationEngine> triangulationEngine;
    SharedHandle<OmegaSLCompiler> omegaSlCompiler;
};

/**
  @brief Init-time options for the Graphics and Triangulation Engine.
  Resolved once at @c Init() and frozen for the process lifetime. See
  gte/docs/Debug-Layer-Plan.md for the cross-backend rollout plan.
 */
struct OMEGAGTE_EXPORT GTEInitOptions {
    /// Tri-state toggle for the backend's debug/validation layer plus
    /// the @c DEBUG_STREAM logging macro.
    enum class DebugLayer : uint8_t {
        Default,   ///< Follow the @c OMEGAGTE_DEBUG compile flag.
        Enabled,   ///< Force on — backend validation + verbose logging.
        Disabled,  ///< Force off — release perf, silent logs.
    };
    DebugLayer debugLayer = DebugLayer::Default;

    /// Enable GPU-Based Validation (D3D12 GBV / Vulkan GPU-assisted).
    /// Heavy — 5–10x draw-call cost. Ignored if debugLayer resolves off.
    bool gpuBasedValidation = false;

    /// Metal only: when true AND @c debugLayer resolves on, start a
    /// programmatic GPU frame capture at @c Init() and stop it at
    /// @c Close(), writing a @c .gputrace document. No-op on other
    /// backends. Gated behind its own flag (not just @c debugLayer)
    /// because traces grow fast. Requires the embedding app to set
    /// @c MetalCaptureEnabled=YES in its Info.plist (or
    /// @c MTL_CAPTURE_ENABLED=1); otherwise capture is unsupported and
    /// silently skipped. See gte/docs/Metal-Debug.md.
    bool captureOnInit = false;

    /// Optional output path for the capture document. When null/empty a
    /// default @c omegagte-<pid>-<timestamp>.gputrace is written to the
    /// working directory. Ignored unless @c captureOnInit is set.
    const char *captureFilePath = nullptr;

    /// Threshold for *gated* emits (@c Error / @c Info / @c Trace): a line
    /// emits only when its level is at or above this floor. @c Critical
    /// bypasses the floor entirely (see @ref DebugLogLevel), so setting
    /// @c logLevel to @c Critical is rejected at @c Init() and clamped up to
    /// @c Error — a @c Critical floor would silence Error/Info/Trace while
    /// changing nothing about Critical. Default @c Info.
    DebugLogLevel logLevel = DebugLogLevel::Info;

    /// Bitmask of @c DEBUG_DOMAIN_* bits allowed to emit. Applies to gated
    /// *and* Critical lines — masking a domain out suppresses even its
    /// Critical reports, which is the one documented "I know this surface
    /// misuses the API, hide it" knob. Default @c ~0u (all domains).
    uint32_t logDomains = ~0u;
};

/**
  @brief Whether the debug layer is active for this process.
  @paragraph Resolved once at @c Init() from @c GTEInitOptions::debugLayer
  and the @c OMEGAGTE_DEBUG compile flag. Backends consult this to decide
  whether to enable native validation; the @c DEBUG_STREAM macro consults
  this to gate log output. Defaults to the compile-time default before
  @c Init() runs.
 */
OMEGAGTE_EXPORT bool isDebugLayerEnabled();

/// @brief Whether GPU-Based Validation was requested at @c Init() time.
/// Always false if @c isDebugLayerEnabled() is false.
OMEGAGTE_EXPORT bool isGpuBasedValidationEnabled();

/// @brief Whether programmatic GPU capture was requested at @c Init() time.
/// Metal only; always false if @c isDebugLayerEnabled() is false.
OMEGAGTE_EXPORT bool isCaptureOnInitEnabled();

/// @brief Resolved capture output path. Empty string means "use the
/// default name". Valid for the process lifetime; never null.
OMEGAGTE_EXPORT const char *captureOutputPath();

OMEGAGTE_EXPORT GTE Init(SharedHandle<GTEDevice> & device, GTEInitOptions opts = {});

OMEGAGTE_EXPORT GTE InitWithDefaultDevice(GTEInitOptions opts = {});

OMEGAGTE_EXPORT void Close(GTE &gte);

_NAMESPACE_END_

#endif // OMEGAGTE_GTEDEVICE_H
