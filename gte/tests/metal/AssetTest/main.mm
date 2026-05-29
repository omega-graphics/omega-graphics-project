// AssetTest — exercises the Phase 3 asset loaders end-to-end on Metal.
//
// Verifies:
//   1. TextureAsset::load loads a PNG and produces a non-null GETexture
//      with the expected dimensions / pixel format.
//   2. A triangulated GEMesh built with a TypeTexture2D attachment that
//      carries the loaded GETexture has that texture in its
//      textureBindings[0] after construction.
//   3. MeshAsset::load loads a tiny OBJ cube and produces a non-null
//      GEMesh with at least one triangle.
//
// The PNG and OBJ test fixtures are written to a temp directory at
// startup so the test is self-contained; no on-disk resources to ship.
//
// Exits 0 on success, non-zero on first failure.

#include <omegaGTE/GTEDevice.h>
#include <omegaGTE/GECommandQueue.h>
#include <omegaGTE/GEPipeline.h>
#include <omegaGTE/GERenderTarget.h>
#include <omegaGTE/GTEMath.h>
#include <omegaGTE/GETextureAsset.h>
#include <omegaGTE/GEMeshAsset.h>
#include <omegaGTE/GEMesh.h>
#include <omegaGTE/TE.h>

#import <Foundation/Foundation.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

namespace {

// Minimal valid PNG: 1x1 RGB red pixel.
const uint8_t kRedPixelPng[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,
    0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x02,0x00,0x00,0x00,0x90,
    0x77,0x53,0xde,0x00,0x00,0x00,0x0c,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,
    0xcf,0xc0,0x00,0x00,0x03,0x01,0x01,0x00,0xc9,0xfe,0x92,0xef,0x00,0x00,0x00,
    0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82,
};

// A minimal triangle in OBJ form. Three vertices, one face. No
// material, no textures — just enough to verify MeshAsset can produce a
// GEMesh from a real file format.
const char *kTriangleObj =
    "v 0.0 0.0 0.0\n"
    "v 1.0 0.0 0.0\n"
    "v 0.0 1.0 0.0\n"
    "vn 0.0 0.0 1.0\n"
    "f 1//1 2//1 3//1\n";

std::string writeTempFile(const std::string & basename,
                          const void * data, size_t len) {
    NSString *tmp = NSTemporaryDirectory();
    NSString *path = [tmp stringByAppendingPathComponent:
                     [NSString stringWithUTF8String:basename.c_str()]];
    std::string cpath = path.UTF8String;
    std::ofstream f(cpath, std::ios::binary);
    f.write(reinterpret_cast<const char *>(data), (std::streamsize)len);
    f.close();
    return cpath;
}

#define EXPECT(cond, msg) do { \
    if (!(cond)) { \
        std::cerr << "[AssetTest] FAIL: " << (msg) \
                  << " (at " << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        std::exit(1); \
    } \
    std::cout << "[AssetTest]  ok : " << (msg) << std::endl; \
} while(0)

}  // namespace

int main(int /*argc*/, const char * /*argv*/[]) {
    auto gte = OmegaGTE::InitWithDefaultDevice();
    std::cout << "[AssetTest] GTE initialized" << std::endl;

    // 1. TextureAsset.
    std::string pngPath = writeTempFile("omega-asset-test-red.png",
                                        kRedPixelPng, sizeof(kRedPixelPng));
    std::cout << "[AssetTest] PNG fixture: " << pngPath << std::endl;

    auto texAsset = OmegaGTE::GETextureAsset::Create(gte.graphicsEngine);
    EXPECT(texAsset != nullptr, "GETextureAsset::Create returned non-null");

    OmegaGTE::GETextureAsset::LoadOptions topts;
    topts.generateMipmaps = false;  // 1x1 — nothing to mip
    topts.sRGB = false;
    bool tloaded = texAsset->load(pngPath, topts);
    EXPECT(tloaded, "TextureAsset::load returned true");

    auto loadedTex = texAsset->texture();
    EXPECT(loadedTex != nullptr, "TextureAsset::texture() is non-null");

    auto tdesc = texAsset->descriptor();
    EXPECT(tdesc.width == 1, "loaded texture width is 1");
    EXPECT(tdesc.height == 1, "loaded texture height is 1");

    // 2. GEMesh built from triangulation, with the loaded texture as
    // the attachment's GPU texture.
    OmegaGTE::GRect rect{};
    rect.h = 100;
    rect.w = 100;
    rect.pos.x = 0;
    rect.pos.y = 0;

    auto params = OmegaGTE::TETriangulationParams::Rect(rect);
    auto attachment = OmegaGTE::TETriangulationParams::Attachment::makeTexture2D(
        100, 100, loadedTex);
    params.addAttachment(attachment);

    // Headless test: build a small offscreen texture and route the
    // triangulation context through it.
    OmegaGTE::TextureDescriptor rtTexDesc;
    rtTexDesc.kind = OmegaGTE::TextureKind::Tex2D;
    rtTexDesc.usage = OmegaGTE::GETexture::RenderTarget;
    rtTexDesc.pixelFormat = OmegaGTE::TexturePixelFormat::RGBA8Unorm;
    rtTexDesc.width = 256;
    rtTexDesc.height = 256;
    auto rtTex = gte.graphicsEngine->makeTexture(rtTexDesc);
    EXPECT(rtTex != nullptr, "offscreen render-target texture allocated");

    OmegaGTE::TextureRenderTargetDescriptor rtDesc;
    rtDesc.renderToExistingTexture = true;
    rtDesc.texture = rtTex;
    rtDesc.region = OmegaGTE::TextureRegion{0, 0, 0, 256, 256, 1};
    auto rt = gte.graphicsEngine->makeTextureRenderTarget(rtDesc);
    EXPECT(rt != nullptr, "texture render target created");

    auto tessQueue = gte.graphicsEngine->makeCommandQueue(64);
    auto tessContext = gte.triangulationEngine->createTEContextFromTextureRenderTarget(rt, tessQueue);
    EXPECT(tessContext != nullptr, "triangulation context created");
    auto result = tessContext->triangulateSync(params);
    EXPECT(result.totalVertexCount() == 6, "rect triangulation produces 6 vertices");

    OmegaGTE::GEMeshDescriptor mdesc;
    mdesc.attributes = OmegaGTE::GEMeshAttrPosition | OmegaGTE::GEMeshAttrUV2;
    mdesc.topology = OmegaGTE::GEMeshTopology::Triangle;
    mdesc.indexType = OmegaGTE::GEMeshIndexType::None;

    auto triMesh = OmegaGTE::buildMeshFromTriangulation(
        gte.graphicsEngine.get(), result, mdesc, loadedTex, /*diffuseSlot*/0);
    EXPECT(triMesh != nullptr, "buildMeshFromTriangulation returned non-null");
    EXPECT(triMesh->vertexCount == 6, "GEMesh has 6 vertices");
    EXPECT(triMesh->vertexBuffer != nullptr, "GEMesh has a vertex buffer");
    EXPECT(triMesh->textureBindings.count(0) == 1, "GEMesh has binding at slot 0");
    EXPECT(triMesh->textureBindings[0] == loadedTex,
           "GEMesh binding[0] points at the TextureAsset's texture");

    // 3. MeshAsset on a tiny OBJ.
    std::string objPath = writeTempFile("omega-asset-test-tri.obj",
                                        kTriangleObj, std::strlen(kTriangleObj));
    std::cout << "[AssetTest] OBJ fixture: " << objPath << std::endl;

    auto meshAsset = OmegaGTE::GEMeshAsset::Create(gte.graphicsEngine);
    EXPECT(meshAsset != nullptr, "GEMeshAsset::Create returned non-null");

    OmegaGTE::GEMeshAsset::LoadOptions mopts;
    mopts.desiredDescriptor.attributes =
        OmegaGTE::GEMeshAttrPosition | OmegaGTE::GEMeshAttrNormal;
    mopts.desiredDescriptor.topology = OmegaGTE::GEMeshTopology::Triangle;
    mopts.desiredDescriptor.indexType = OmegaGTE::GEMeshIndexType::None;
    mopts.loadMaterialTextures = false;  // OBJ here has no material

    bool mloaded = meshAsset->load(objPath, mopts);
    EXPECT(mloaded, "MeshAsset::load returned true");

    auto loadedMesh = meshAsset->mesh();
    EXPECT(loadedMesh != nullptr, "MeshAsset::mesh() is non-null");
    // One triangle = 3 vertices in the flat output.
    EXPECT(loadedMesh->vertexCount == 3, "MeshAsset produced 3 vertices");
    EXPECT(loadedMesh->vertexBuffer != nullptr, "MeshAsset GEMesh has a vertex buffer");

    // 4. MeshAsset on an FBX cube. Model I/O can't load FBX, so this exercises
    //    the .fbx → shared MeshParser (ufbx) route added for Metal. The file
    //    is one of ufbx's bundled test cubes, located via a compile-defined
    //    path. A cube's six quads triangulate to 12 triangles = 36 vertices.
#ifdef OMEGA_ASSET_TEST_FBX
    std::string fbxPath = OMEGA_ASSET_TEST_FBX;
    std::cout << "[AssetTest] FBX fixture: " << fbxPath << std::endl;

    auto fbxAsset = OmegaGTE::GEMeshAsset::Create(gte.graphicsEngine);
    EXPECT(fbxAsset != nullptr, "GEMeshAsset::Create (FBX) returned non-null");

    OmegaGTE::GEMeshAsset::LoadOptions fopts;
    fopts.desiredDescriptor.attributes =
        OmegaGTE::GEMeshAttrPosition | OmegaGTE::GEMeshAttrNormal;
    fopts.desiredDescriptor.topology = OmegaGTE::GEMeshTopology::Triangle;
    fopts.desiredDescriptor.indexType = OmegaGTE::GEMeshIndexType::None;
    fopts.loadMaterialTextures = false;

    bool floaded = fbxAsset->load(fbxPath, fopts);
    EXPECT(floaded, "MeshAsset::load (FBX) returned true");

    auto fbxMesh = fbxAsset->mesh();
    EXPECT(fbxMesh != nullptr, "MeshAsset::mesh() (FBX) is non-null");
    EXPECT(fbxMesh->vertexBuffer != nullptr, "FBX GEMesh has a vertex buffer");
    // Triangulated stream: a whole number of triangles, and a cube yields 36.
    EXPECT(fbxMesh->vertexCount > 0 && fbxMesh->vertexCount % 3 == 0,
           "FBX GEMesh vertex count is a positive multiple of 3");
    EXPECT(fbxMesh->vertexCount == 36, "FBX cube produced 36 vertices (12 triangles)");
#else
    std::cout << "[AssetTest] (FBX case skipped: OMEGA_ASSET_TEST_FBX not defined)" << std::endl;
#endif

    std::cout << "[AssetTest] all checks passed" << std::endl;
    return 0;
}
