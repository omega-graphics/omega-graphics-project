#include "../GTETestEntryPoint.h"
#include "../GTETestWindow.h"

// MeshAndRaytracingTest — G-buffer raster + RAY-TRACED SHADOWS.
//
// The test finally does what its name promised: it rasterizes a small 3D scene and
// then ray-traces the shadows in it, so a broken acceleration structure fails
// VISIBLY (missing or wrong shadows) instead of silently returning zeros that a
// readback assert would happily accept.
//
// The scene:
//   - A flattened cylinder acting as a floor — the surface the shadows land ON. A
//     shadow test with nothing to receive shadows proves nothing.
//   - Four TE 3D primitives (Sphere, Cylinder, Cone, Torus) hovering above it,
//     placed by GESpace. These are the casters.
//   - The GEMeshAsset-loaded FBX, drawn through the mesh-shader pipeline, also a
//     caster.
//
// The frame, in three passes:
//   1. G-BUFFER (offscreen, MRT + depth). Rasterize everything into RGBA32Float
//      world position + RGBA16Float world normal + RGBA8 albedo, with a real
//      D32Float depth buffer so nearer surfaces actually win. This is the pass that
//      the engine could not express until depth attachments and float formats
//      landed.
//   2. SHADOW (compute). For each covered pixel, read its world position/normal,
//      cast ONE shadow ray at the light through the TLAS with
//      RAY_FLAG_TERMINATE_ON_FIRST_HIT, and shade.
//   3. COMPOSITE. Resolve the lit image onto the drawable with a fullscreen
//      triangle.
//
// Why offscreen at all: a drawable is BGRA8Unorm. World positions are unbounded and
// normals are signed — neither survives 8-bit unorm — and no API gives a swapchain
// a depth buffer anyway. So 3D renders into texture targets that own their depth,
// and only the finished image is resolved to the drawable. That is now the
// engine-wide standard (see PixelFormat-Completion-Plan.md).
//
// Acceleration structures: one BLAS per drawn object, built over the SAME vertex
// buffer the raster pass consumes (that is what the TriangleList stride/count fields
// are for — a Position+Normal vertex is 24/32B, not the tightly-packed 12B a BLAS
// used to assume), and one TLAS instancing them with each object's GESpace
// local→world transform. The shadow kernel traces that TLAS.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GETexture.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GTEShader.h>
#include <omegaGTE/GEMesh.h>
#include <omegaGTE/GEMeshAsset.h>
#include <omegaGTE/GESpace.h>
#include <omegaGTE/TE.h>

#include <omega-common/fs.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <vector>

using namespace OmegaGTE;

// Triangles per amplification threadgroup. Must match the `32u` in the shader's
// `ampFunc`.
static constexpr uint32_t kTrianglesPerBatch = 32;

static constexpr float kWindowWidth  = 800.f;
static constexpr float kWindowHeight = 600.f;

// Depth range of the SPACE, in space units (NOT the rasterizer's [0,1] range).
static constexpr float kSpaceNearDepth = -1000.f;
static constexpr float kSpaceFarDepth  =  1000.f;

static constexpr float kFillFraction = 0.45f;

// ── Push-constant blocks (must match the shader byte for byte) ──────────────

/// `constant<ObjectXform> pc : 0` — three column-major float4x4 plus an albedo and
/// the triangle count. std430: mat4s are 64B each at 16B alignment, the float4 is
/// 16B, the uint is 4B.
struct ObjectXformConstants {
    float    mvp[16];
    float    model[16];
    float    normalMat[16];
    float    albedo[4];
    uint32_t triCount;
};

/// `constant<ShadowParams> sp : 3`.
struct ShadowParamsConstants {
    float lightPos[4];
    float ambient[4];   // rgb = ambient, w = shadow strength
    float dims[4];      // xy = G-buffer extent
};

// ── Scene ───────────────────────────────────────────────────────────────────

/// One drawable thing: where it lives (GESpace), what geometry it has (GEMesh), and
/// the BLAS built over that geometry.
struct SceneObject {
    GESpaceObjectID          id = GESpaceInvalidObject;
    SharedHandle<GEMesh>     mesh;
    SharedHandle<GEAccelerationStruct> blas;
    float                    albedo[4] = {0.8f, 0.8f, 0.8f, 1.f};
    bool                     isFBX = false;   // drawn with the mesh pipeline
};

static GTE gte;
static SharedHandle<GTEShaderLibrary> library;
static SharedHandle<GECommandQueue>   commandQueue;
static SharedHandle<GENativeRenderTarget> renderTarget;

static SharedHandle<GERenderPipelineState>  primPipeline;
static SharedHandle<GERenderPipelineState>  meshPipeline;
static SharedHandle<GEComputePipelineState> shadowPipeline;
static SharedHandle<GERenderPipelineState>  compositePipeline;

// G-buffer surfaces + the render target that owns them (and the depth buffer).
static SharedHandle<GETexture> gWorldPosTex, gNormalTex, gAlbedoTex, gDepthTex, litTex;
static SharedHandle<GETextureRenderTarget> gWorldPosRT, gNormalRT, gAlbedoRT;

static SharedHandle<GEAccelerationStruct> tlas;

static UniqueHandle<GESpace> space;
static std::vector<SceneObject> sceneObjects;
static SharedHandle<GEMeshAsset> meshAsset;
static SharedHandle<OmegaTriangulationEngineContext> teContext;

/// Flatten an FMatrix<4,4> into 16 floats. FMatrix stores column-major and so does
/// the shader's `float4x4`, so column `c` goes to floats [4c..4c+3] — no transpose.
/// Getting this backwards is SILENT: a transposed matrix still draws a
/// plausible-looking picture, just the wrong one.
static void flattenColumnMajor(const FMatrix<4,4> &m, float out[16]) {
    for (unsigned c = 0; c < 4; ++c) {
        for (unsigned r = 0; r < 4; ++r) {
            out[c * 4 + r] = m[c][r];
        }
    }
}

/// The inverse-transpose of a model matrix's upper 3x3, written back as a 4x4.
///
/// Rotating a normal by the model matrix is only correct under UNIFORM scale. These
/// objects are non-uniformly scaled (the floor especially — it is a cylinder
/// squashed flat), and under that a plain model-matrix rotation tilts normals off
/// the surface, so the lighting and the shadow term both go quietly wrong.
static void normalMatrixOf(const FMatrix<4,4> &model, float out[16]) {
    // Upper-left 3x3.
    float m[3][3];
    for (unsigned c = 0; c < 3; ++c) {
        for (unsigned r = 0; r < 3; ++r) {
            m[r][c] = model[c][r];
        }
    }

    const float det =
        m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
        m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
        m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);

    // Degenerate (a zero scale on some axis) — fall back to identity rather than
    // dividing by zero and spraying NaNs through every normal in the frame.
    if (std::abs(det) < 1e-8f) {
        for (unsigned i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.f : 0.f;
        return;
    }
    const float invDet = 1.f / det;

    // inverse(m), then transpose — done in one step below.
    float inv[3][3];
    inv[0][0] =  (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * invDet;
    inv[0][1] = -(m[0][1] * m[2][2] - m[0][2] * m[2][1]) * invDet;
    inv[0][2] =  (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * invDet;
    inv[1][0] = -(m[1][0] * m[2][2] - m[1][2] * m[2][0]) * invDet;
    inv[1][1] =  (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * invDet;
    inv[1][2] = -(m[0][0] * m[1][2] - m[0][2] * m[1][0]) * invDet;
    inv[2][0] =  (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * invDet;
    inv[2][1] = -(m[0][0] * m[2][1] - m[0][1] * m[2][0]) * invDet;
    inv[2][2] =  (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * invDet;

    // out = transpose(inv), stored column-major: out[c*4+r] = transpose(inv)[r][c]
    //                                                       = inv[c][r].
    for (unsigned i = 0; i < 16; ++i) out[i] = 0.f;
    for (unsigned c = 0; c < 3; ++c) {
        for (unsigned r = 0; r < 3; ++r) {
            out[c * 4 + r] = inv[c][r];
        }
    }
    out[15] = 1.f;
}

/// Build the BLAS for one object's mesh.
///
/// The vertex buffer is handed over WITH its stride and vertex count, because these
/// meshes carry Position+Normal (a 24- or 32-byte vertex). The BLAS reads only the
/// position at offset 0, but it has to be told how far apart the vertices are — the
/// old "tightly-packed float3" assumption would step 12 bytes at a time through a
/// 32-byte vertex and trace shredded geometry.
static SharedHandle<GEAccelerationStruct> buildBLAS(const SharedHandle<GEMesh> &mesh) {
    if (!mesh || !mesh->vertexBuffer || mesh->vertexCount < 3) {
        return nullptr;
    }
    GEAccelerationStructDescriptor desc;
    desc.level = GEAccelerationStructDescriptor::BottomLevel;
    desc.addTriangleBuffer(mesh->vertexBuffer,
                           mesh->vertexStride,
                           mesh->vertexCount);
    return gte.graphicsEngine->allocateAccelerationStructure(desc);
}

/// Place a TE primitive in the space and build its GPU mesh WITH NORMALS.
///
/// `GEMeshAttrPosition | GEMeshAttrNormal` is the opt-in that Raytracing plan §7-V.a
/// unlocked: TE was already computing a normal per vertex and then throwing it away
/// whenever no UV/color attachment happened to be requested, which is exactly what
/// GESpace's primitive path does. Position-only is still the DEFAULT — this asks for
/// more, and now actually gets it instead of a buffer of zeros.
static SceneObject addPrimitive(TETriangulationParams params,
                                const GPoint3D &translation,
                                const GPoint3D &scale,
                                const float albedo[4]) {
    SceneObject obj;
    obj.id = space->addPrimitive(teContext.get(), params);
    if (obj.id == GESpaceInvalidObject) {
        std::cerr << "[MeshAndRaytracingTest] addPrimitive failed" << std::endl;
        return obj;
    }
    space->setScale(obj.id, scale);
    space->setTranslation(obj.id, translation);

    GEMeshDescriptor md{};
    md.attributes = GEMeshAttrPosition | GEMeshAttrNormal;
    md.topology   = GEMeshTopology::Triangle;
    md.indexType  = GEMeshIndexType::None;

    obj.mesh = space->meshOf(obj.id, gte.graphicsEngine.get(), md);
    if (!obj.mesh) {
        std::cerr << "[MeshAndRaytracingTest] meshOf(primitive) failed" << std::endl;
        return obj;
    }
    obj.blas = buildBLAS(obj.mesh);
    std::copy(albedo, albedo + 4, obj.albedo);
    return obj;
}

/// Create the offscreen G-buffer + the lit output.
static bool createTargets(unsigned w, unsigned h) {
    auto mk = [&](PixelFormat fmt, GETexture::GETextureUsage usage) {
        TextureDescriptor d{};
        d.storage_opts = GPUOnly;
        d.usage  = usage;
        d.pixelFormat = fmt;
        d.width  = w;
        d.height = h;
        d.depth  = 1;
        d.kind   = TextureKind::Tex2D;
        return gte.graphicsEngine->makeTexture(d);
    };

    // World position needs full float — positions are unbounded. Normals are
    // signed, so they need a float format too (half is plenty). Albedo is the only
    // thing that fits in 8-bit unorm.
    gWorldPosTex = mk(PixelFormat::RGBA32Float, GETexture::RenderTarget);
    gNormalTex   = mk(PixelFormat::RGBA16Float, GETexture::RenderTarget);
    gAlbedoTex   = mk(PixelFormat::RGBA8Unorm,  GETexture::RenderTarget);
    gDepthTex    = mk(PixelFormat::D32Float,    GETexture::RenderTargetAndDepthStencil);
    // The compute pass writes this, the composite pass samples it.
    litTex       = mk(PixelFormat::RGBA8Unorm,  GETexture::GPUAccessOnly);

    if (!gWorldPosTex || !gNormalTex || !gAlbedoTex || !gDepthTex || !litTex) {
        std::cerr << "[MeshAndRaytracingTest] G-buffer texture creation failed" << std::endl;
        return false;
    }

    auto mkRT = [&](const SharedHandle<GETexture> &tex,
                    const SharedHandle<GETexture> &depth) {
        TextureRenderTargetDescriptor rd{};
        rd.renderToExistingTexture = true;
        rd.texture = tex;
        rd.depthTexture = depth;   // null for the secondary MRT slots
        rd.region = TextureRegion{0, 0, 0, w, h, 1};
        return gte.graphicsEngine->makeTextureRenderTarget(rd);
    };

    // Only attachment 0's render target carries the depth surface: a pass has
    // exactly ONE depth attachment, and it is taken from the target backing color
    // attachment 0.
    gWorldPosRT = mkRT(gWorldPosTex, gDepthTex);
    gNormalRT   = mkRT(gNormalTex,   nullptr);
    gAlbedoRT   = mkRT(gAlbedoTex,   nullptr);

    if (!gWorldPosRT || !gNormalRT || !gAlbedoRT) {
        std::cerr << "[MeshAndRaytracingTest] G-buffer render target creation failed" << std::endl;
        return false;
    }
    return true;
}

/// Build the TLAS over every object's BLAS, using each object's GESpace local→world
/// transform. The shadow kernel traces this.
static bool buildTLAS() {
    GEAccelerationStructDescriptor td;

    unsigned instanceCount = 0;
    for (auto &obj : sceneObjects) {
        if (!obj.blas) continue;

        // GESpace stores column-major; the TLAS instance transform is a 3x4
        // ROW-major affine matrix (the implicit last row is [0 0 0 1]). So
        // transform[r][c] = model[c][r].
        const auto model = space->transformOf(obj.id).modelMatrix();
        float xf[3][4];
        for (unsigned r = 0; r < 3; ++r) {
            for (unsigned c = 0; c < 4; ++c) {
                xf[r][c] = model[c][r];
            }
        }
        td.addInstance(obj.blas, xf, /*mask=*/0xFF, /*instanceID=*/instanceCount);
        ++instanceCount;
    }

    if (instanceCount == 0) {
        std::cerr << "[MeshAndRaytracingTest] no BLAS to instance; TLAS skipped" << std::endl;
        return false;
    }

    tlas = gte.graphicsEngine->allocateAccelerationStructure(td);
    if (!tlas) {
        std::cerr << "[MeshAndRaytracingTest] TLAS allocation failed" << std::endl;
        return false;
    }
    std::cout << "[MeshAndRaytracingTest] TLAS: " << instanceCount << " instances" << std::endl;
    return true;
}

GTE_TEST_ENTRY_POINT {
    (void)argc;

    OmegaCommon::FS::changeCWD(OmegaCommon::FS::getExecutableDir());

    gte = OmegaGTE::InitWithDefaultDevice();
    std::cout << "[MeshAndRaytracingTest] GTE Initialized" << std::endl;

    // ── Feature gates ───────────────────────────────────────────────
    bool meshSupported = false;
    bool rtSupported   = false;
    for (auto &dev : OmegaGTE::enumerateDevices()) {
        if (!dev) continue;
        if (dev->features.hasFeature(GTEDEVICE_FEATURE_MESH_SHADER))  meshSupported = true;
        if (dev->features.hasFeature(GTEDEVICE_FEATURE_RAYTRACING))   rtSupported   = true;
    }
    std::cout << "[MeshAndRaytracingTest] MESH_SHADER = " << (meshSupported ? "YES" : "NO")
              << ", RAYTRACING = " << (rtSupported ? "YES" : "NO") << std::endl;
    if (!rtSupported) {
        std::cerr << "[MeshAndRaytracingTest] device has no raytracing; nothing to show." << std::endl;
        return 0;   // not a failure — just not this machine
    }

    library = gte.graphicsEngine->loadShaderLibrary("./meshAndRaytracing.omegasllib");
    if (!library) {
        std::cerr << "[MeshAndRaytracingTest] failed to load ./meshAndRaytracing.omegasllib" << std::endl;
        return 1;
    }

    // ── G-buffer surfaces ───────────────────────────────────────────
    const auto W = static_cast<unsigned>(kWindowWidth);
    const auto H = static_cast<unsigned>(kWindowHeight);
    if (!createTargets(W, H)) return 1;

    // ── Pipelines ───────────────────────────────────────────────────
    //
    // The G-buffer pipelines declare all THREE color formats and the depth format.
    // Metal will not test depth at all unless the pipeline names the depth format —
    // a depth-enabled pipeline without it is silently inert.
    const OmegaCommon::Vector<PixelFormat> gbufFormats = {
        PixelFormat::RGBA32Float, PixelFormat::RGBA16Float, PixelFormat::RGBA8Unorm
    };

    RenderPipelineDescriptor primDesc{};
    primDesc.name = "MeshAndRaytracing.GBuffer.Primitives";
    primDesc.vertexFunc   = library->shaders["primVert"];
    primDesc.fragmentFunc = library->shaders["primFrag"];
    primDesc.colorPixelFormats = gbufFormats;
    primDesc.depthStencilPixelFormat = PixelFormat::D32Float;
    primDesc.depthAndStencilDesc.enableDepth   = true;
    primDesc.depthAndStencilDesc.depthOperation = CompareFunc::Less;
    primPipeline = gte.graphicsEngine->makeRenderPipelineState(primDesc);

    MeshPipelineDescriptor meshDesc{};
    meshDesc.name = "MeshAndRaytracing.GBuffer.Mesh";
    meshDesc.amplificationFunc = library->shaders["ampFunc"];
    meshDesc.meshFunc          = library->shaders["meshFunc"];
    meshDesc.fragmentFunc      = library->shaders["meshFrag"];
    meshDesc.colorPixelFormats = gbufFormats;
    meshDesc.depthStencilPixelFormat = PixelFormat::D32Float;
    meshDesc.depthAndStencilDesc.enableDepth    = true;
    meshDesc.depthAndStencilDesc.depthOperation = CompareFunc::Less;
    meshPipeline = meshSupported ? gte.graphicsEngine->makeMeshPipelineState(meshDesc) : nullptr;

    ComputePipelineDescriptor shadowDesc{};
    shadowDesc.name = "MeshAndRaytracing.ShadowKernel";
    shadowDesc.computeFunc = library->shaders["shadowKernel"];
    shadowPipeline = gte.graphicsEngine->makeComputePipelineState(shadowDesc);

    RenderPipelineDescriptor compDesc{};
    compDesc.name = "MeshAndRaytracing.Composite";
    compDesc.vertexFunc   = library->shaders["compositeVert"];
    compDesc.fragmentFunc = library->shaders["compositeFrag"];
    compDesc.colorPixelFormats = { PixelFormat::BGRA8Unorm };   // the drawable
    compDesc.depthAndStencilDesc.enableDepth = false;
    compositePipeline = gte.graphicsEngine->makeRenderPipelineState(compDesc);

    std::cout << "[MeshAndRaytracingTest] pipelines: prim=" << (primPipeline ? "ok" : "NULL")
              << " mesh=" << (meshPipeline ? "ok" : "NULL")
              << " shadow=" << (shadowPipeline ? "ok" : "NULL")
              << " composite=" << (compositePipeline ? "ok" : "NULL") << std::endl;
    if (!primPipeline || !shadowPipeline || !compositePipeline) {
        std::cerr << "[MeshAndRaytracingTest] a required pipeline is null; aborting." << std::endl;
        return 1;
    }

    // ── Window / frame ──────────────────────────────────────────────
    OmegaGTETests::GTETestWindowDescriptor desc;
    desc.title  = "GTE MeshAndRaytracingTest — RT shadows";
    desc.width  = W;
    desc.height = H;

    OmegaGTETests::GTETestWindowDelegate del;

    del.onReady = [W, H, meshSupported](const NativeRenderTargetDescriptor &nrt) {
    GECommandQueueDesc qd{};
    qd.maxBufferCount = 64;
    commandQueue = gte.graphicsEngine->makeCommandQueue(qd);
    renderTarget = gte.graphicsEngine->makeNativeRenderTarget(nrt, commandQueue);

    // ── Scene ───────────────────────────────────────────────────────
    //
    // Built here rather than up front because a TE context binds to a render
    // target + queue, and neither exists until the window is ready.
    GEViewport spaceViewport;
    spaceViewport.x = 0.f;  spaceViewport.y = 0.f;
    spaceViewport.width  = kWindowWidth;
    spaceViewport.height = kWindowHeight;
    spaceViewport.nearDepth = kSpaceNearDepth;
    spaceViewport.farDepth  = kSpaceFarDepth;
    space = std::make_unique<GESpace>(spaceViewport);

    // ── Camera ──────────────────────────────────────────────────────
    //
    // A real perspective camera, looking DOWN at the scene from in front.
    //
    // The first cut of this test used the space's default viewport-linear
    // (orthographic, axis-aligned) map, and it made the shadows invisible: the
    // floor is a horizontal disc in the XZ plane, and an orthographic camera
    // staring straight down -Z sees a horizontal plane EDGE-ON — it rendered as a
    // 1-pixel line. The shadows were almost certainly being traced correctly; they
    // just had nowhere visible to land.
    //
    // Setting a view + projection here is exactly what GESpace Phase 5 is for: it
    // composes projection·view·model itself, so `objectTransform()` still returns a
    // single local→clip matrix and nothing downstream changes. Note the scene below
    // is now authored in WORLD units (metres-ish), not pixels — with a real lens,
    // pixel-sized coordinates make no sense.
    space->setViewMatrix(lookAt(GPoint3D{0.f, 5.5f, 11.f},    // eye: up and in front
                                GPoint3D{0.f, 1.2f,  0.f},    // target: the objects
                                GPoint3D{0.f, 1.f,   0.f}));  // up
    space->setProjectionMatrix(perspectiveProjection(
        45.f * 3.14159265f / 180.f,
        kWindowWidth / kWindowHeight,
        0.1f, 200.f));

    teContext = gte.triangulationEngine->createTEContextFromTextureRenderTarget(
        gWorldPosRT, commandQueue);
    if (!teContext) {
        std::cerr << "[MeshAndRaytracingTest] TE context creation failed" << std::endl;
        return;
    }
    // Scene in WORLD units now (roughly metres), with the floor at y = 0 and the
    // casters standing on it.

    // The FLOOR — a cylinder squashed flat into a disc lying in the XZ plane. This
    // is what the shadows fall ON; a shadow test with no receiver proves nothing.
    // Deliberately non-uniformly scaled (7 x 0.15 x 7), which is also what makes
    // the inverse-transpose normal matrix necessary rather than decorative.
    {
        GCylinder disc{GPoint3D{0.f, 0.f, 0.f}, 1.f, 1.f};
        auto p = TETriangulationParams::Cylinder(disc);
        const float grey[4] = {0.78f, 0.78f, 0.82f, 1.f};
        sceneObjects.push_back(addPrimitive(p,
            GPoint3D{0.f, 0.f, 0.f},
            GPoint3D{7.f, 0.15f, 7.f},
            grey));
    }

    // The CASTERS, standing on the floor.
    {
        GSphere s{GPoint3D{0.f, 0.f, 0.f}, 1.f};
        auto p = TETriangulationParams::Sphere(s);
        const float red[4] = {0.85f, 0.30f, 0.25f, 1.f};
        sceneObjects.push_back(addPrimitive(p,
            GPoint3D{-2.6f, 1.1f, 0.3f}, GPoint3D{1.1f, 1.1f, 1.1f}, red));
    }
    {
        // Stood UP on its edge (rotated about X) so the hole faces the camera —
        // lying flat it reads as a featureless pill, and a torus casting a shadow
        // with a hole in it is the whole point of putting one here.
        GTorus t{GPoint3D{0.f, 0.f, 0.f}, 1.f, 0.35f};
        auto p = TETriangulationParams::Torus(t);
        const float green[4] = {0.30f, 0.75f, 0.40f, 1.f};
        auto obj = addPrimitive(p,
            GPoint3D{0.2f, 1.5f, -0.4f}, GPoint3D{1.3f, 1.3f, 1.3f}, green);
        if (obj.id != GESpaceInvalidObject) {
            space->rotateAxis(obj.id, 1.f, 0.f, 0.f, 1.5707963f);
        }
        sceneObjects.push_back(obj);
    }
    {
        GCone c{0.f, 0.f, 0.f, 1.f, 2.f};
        auto p = TETriangulationParams::Cone(c);
        const float blue[4] = {0.30f, 0.45f, 0.85f, 1.f};
        sceneObjects.push_back(addPrimitive(p,
            GPoint3D{2.9f, 0.1f, 0.2f}, GPoint3D{1.f, 1.2f, 1.f}, blue));
    }
    {
        GCylinder cyl{GPoint3D{0.f, 0.f, 0.f}, 1.f, 2.f};
        auto p = TETriangulationParams::Cylinder(cyl);
        const float amber[4] = {0.90f, 0.70f, 0.25f, 1.f};
        sceneObjects.push_back(addPrimitive(p,
            GPoint3D{1.3f, 0.8f, 1.9f}, GPoint3D{0.55f, 0.8f, 0.55f}, amber));
    }

    // The FBX, through the mesh-shader pipeline.
    if (meshPipeline) {
        meshAsset = GEMeshAsset::Create(gte.graphicsEngine);
        GEMeshAsset::LoadOptions opts{};
        opts.desiredDescriptor.attributes = GEMeshAttrPosition;   // FBX path stays Position-only
        opts.desiredDescriptor.topology   = GEMeshTopology::Triangle;
        opts.desiredDescriptor.indexType  = GEMeshIndexType::None;
        opts.loadMaterialTextures = false;

        if (meshAsset->load("./orange_tennis_racket.fbx", opts)) {
            SceneObject obj;
            obj.mesh  = meshAsset->mesh();
            obj.isFBX = true;
            if (obj.mesh && obj.mesh->vertexBuffer && obj.mesh->bounds.valid
                && obj.mesh->bounds.longestExtent() > 0.f) {
                obj.id = space->addMesh(obj.mesh);
                // Fit the racket to ~4 world units on its longest axis, stand it
                // upright (it is authored lying in the XY plane), and set it behind
                // the primitives so it casts across the back of the floor.
                const float fit = 4.5f / obj.mesh->bounds.longestExtent();
                const auto ctr = obj.mesh->bounds.center();
                space->setScale(obj.id, GPoint3D{fit, fit, fit});
                space->rotateAxis(obj.id, 1.f, 0.f, 0.f, -1.2f);
                space->setTranslation(obj.id,
                    GPoint3D{-0.3f - ctr.x * fit,
                              2.4f - ctr.y * fit,
                             -3.2f - ctr.z * fit});
                obj.blas = buildBLAS(obj.mesh);
                const float orange[4] = {0.85f, 0.55f, 0.20f, 1.f};
                std::copy(orange, orange + 4, obj.albedo);
                sceneObjects.push_back(obj);
                std::cout << "[MeshAndRaytracingTest] FBX: " << obj.mesh->vertexCount
                          << " verts, stride " << obj.mesh->vertexStride << "B" << std::endl;
            }
        } else {
            std::cerr << "[MeshAndRaytracingTest] FBX load failed; continuing with primitives only."
                      << std::endl;
        }
    }

    if (!buildTLAS()) return;

        auto cb = commandQueue->getAvailableBuffer();

        // ── Pass 0: build the acceleration structures ───────────────
        //
        // BLAS first, then the TLAS that instances them: a TLAS build reads its
        // BLAS, so they cannot be recorded the other way round.
        cb->beginAccelStructPass();
        for (auto &obj : sceneObjects) {
            if (!obj.blas || !obj.mesh) continue;
            GEAccelerationStructDescriptor bd;
            bd.level = GEAccelerationStructDescriptor::BottomLevel;
            bd.addTriangleBuffer(obj.mesh->vertexBuffer,
                                 obj.mesh->vertexStride,
                                 obj.mesh->vertexCount);
            cb->buildAccelerationStructure(obj.blas, bd);
        }
        {
            GEAccelerationStructDescriptor td;
            unsigned n = 0;
            for (auto &obj : sceneObjects) {
                if (!obj.blas) continue;
                const auto model = space->transformOf(obj.id).modelMatrix();
                float xf[3][4];
                for (unsigned r = 0; r < 3; ++r)
                    for (unsigned c = 0; c < 4; ++c)
                        xf[r][c] = model[c][r];
                td.addInstance(obj.blas, xf, 0xFF, n++);
            }
            cb->buildAccelerationStructure(tlas, td);
        }
        cb->finishAccelStructPass();

        // ── Pass 1: G-buffer ────────────────────────────────────────
        using CA = GERenderPassDescriptor::ColorAttachment;
        GERenderPassDescriptor gpass{};
        // Attachment 0's render target is the one that owns the depth surface.
        gpass.tRenderTarget = gWorldPosRT.get();
        // worldPos cleared to w=0 — that is the COVERAGE flag the shadow kernel
        // tests. Clearing it to anything with w!=0 would make the whole background
        // look like geometry sitting at the world origin.
        gpass.colorAttachments.push_back(CA(CA::ClearColor(0.f, 0.f, 0.f, 0.f), CA::Clear));
        gpass.colorAttachments.push_back(CA(CA::ClearColor(0.f, 0.f, 0.f, 0.f), CA::Clear, gNormalRT));
        gpass.colorAttachments.push_back(CA(CA::ClearColor(0.f, 0.f, 0.f, 1.f), CA::Clear, gAlbedoRT));
        gpass.depthStencilAttachment.disabled       = false;
        gpass.depthStencilAttachment.depthloadAction = GERenderPassDescriptor::DepthStencilAttachment::Clear;
        gpass.depthStencilAttachment.clearDepth      = 1.f;

        GEViewport    vp {0, 0, kWindowWidth, kWindowHeight, 0.f, 1.f};
        GEScissorRect sr {0, 0, kWindowWidth, kWindowHeight};

        cb->startRenderPass(gpass);
        cb->setViewports({vp});
        cb->setScissorRects({sr});

        for (auto &obj : sceneObjects) {
            if (!obj.mesh || !obj.mesh->vertexBuffer || obj.id == GESpaceInvalidObject) continue;

            ObjectXformConstants k{};
            const auto model = space->transformOf(obj.id).modelMatrix();
            flattenColumnMajor(space->objectTransform(obj.id), k.mvp);
            flattenColumnMajor(model, k.model);
            normalMatrixOf(model, k.normalMat);
            std::copy(obj.albedo, obj.albedo + 4, k.albedo);
            k.triCount = obj.mesh->vertexCount / 3u;

            if (obj.isFBX) {
                if (!meshPipeline) continue;
                cb->setRenderPipelineState(meshPipeline);
                cb->setRenderConstants(&k, sizeof(k));
                // The mesh stage occupies the vertex slot; register 2 matches
                // `buffer<MeshVertexIn> vertBuf : 2` in the shader.
                cb->bindResourceAtVertexShader(obj.mesh->vertexBuffer, 2);
                const uint32_t batches =
                    (k.triCount + kTrianglesPerBatch - 1u) / kTrianglesPerBatch;
                cb->drawMeshTasks(batches, 1, 1);
            } else {
                cb->setRenderPipelineState(primPipeline);
                cb->setRenderConstants(&k, sizeof(k));
                // register 1 matches `buffer<PrimVertex> primVerts : 1`.
                cb->bindResourceAtVertexShader(obj.mesh->vertexBuffer, 1);
                cb->drawPolygons(GECommandBuffer::Triangle, obj.mesh->vertexCount, 0);
            }
        }
        cb->finishRenderPass();

        // ── Pass 2: ray-traced shadows ──────────────────────────────
        ShadowParamsConstants sp{};
        // World space now. Up, to the left, and in front — so the casters throw
        // their shadows back and to the right across the floor, where the camera
        // can see them.
        sp.lightPos[0] = -5.5f;
        sp.lightPos[1] = 9.0f;
        sp.lightPos[2] =  5.0f;
        // w = the light's RADIUS. Nonzero makes it an area light, which is what
        // gives the shadows a soft penumbra instead of a hard stencil edge. This is
        // where the ray budget goes: kShadowSamples rays per pixel instead of one.
        sp.lightPos[3] = 0.6f;
        sp.ambient[0] = 0.16f; sp.ambient[1] = 0.17f; sp.ambient[2] = 0.21f;
        sp.ambient[3] = 0.90f;   // shadow strength
        sp.dims[0] = static_cast<float>(W);
        sp.dims[1] = static_cast<float>(H);

        GEComputePassDescriptor cpass{};
        cb->startComputePass(cpass);
        cb->setComputePipelineState(shadowPipeline);
        cb->setComputeConstants(&sp, sizeof(sp), 0);
        cb->bindResourceAtComputeShader(tlas, 4);
        cb->bindResourceAtComputeShader(gWorldPosTex, 5);
        cb->bindResourceAtComputeShader(gNormalTex, 6);
        cb->bindResourceAtComputeShader(gAlbedoTex, 7);
        cb->bindResourceAtComputeShader(litTex, 8);
        cb->dispatchThreadgroups((W + 7u) / 8u, (H + 7u) / 8u, 1);
        cb->finishComputePass();

        // ── Pass 3: composite onto the drawable ─────────────────────
        GERenderPassDescriptor cpassDesc{};
        cpassDesc.nRenderTarget = renderTarget.get();
        cpassDesc.colorAttachments.push_back(
            CA(CA::ClearColor(0.05f, 0.06f, 0.09f, 1.f), CA::Clear));

        cb->startRenderPass(cpassDesc);
        cb->setViewports({vp});
        cb->setScissorRects({sr});
        cb->setRenderPipelineState(compositePipeline);
        cb->bindResourceAtFragmentShader(litTex, 9);
        // Fullscreen TRIANGLE: 3 vertices, no vertex buffer — positions come from
        // the vertex id.
        cb->drawPolygons(GECommandBuffer::Triangle, 3, 0);
        cb->finishRenderPass();

        commandQueue->submitCommandBuffer(cb);
        commandQueue->commitToGPU();
        renderTarget->present();

        std::cout << "[MeshAndRaytracingTest] frame submitted" << std::endl;
    };

    del.onClose = []() {
        commandQueue->commitToGPUAndWait();

        sceneObjects.clear();
        tlas.reset();
        space.reset();
        teContext.reset();
        meshAsset.reset();

        litTex.reset(); gDepthTex.reset();
        gAlbedoRT.reset(); gNormalRT.reset(); gWorldPosRT.reset();
        gAlbedoTex.reset(); gNormalTex.reset(); gWorldPosTex.reset();

        compositePipeline.reset(); shadowPipeline.reset();
        meshPipeline.reset(); primPipeline.reset();
        renderTarget.reset();
        library.reset();
        commandQueue.reset();

        OmegaGTE::Close(gte);
    };

    return OmegaGTETests::RunGTETestWindow(argc, argv, desc, del);
};
