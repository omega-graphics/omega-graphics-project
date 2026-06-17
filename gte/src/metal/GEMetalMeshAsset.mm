#include "omegaGTE/GEMeshAsset.h"
#include "omegaGTE/GEMesh.h"
#include "omegaGTE/GETextureAsset.h"
#include "omegaGTE/GTEShader.h"
#include "GEMetal.h"
#include "../common/MeshParser.h"

#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <ModelIO/ModelIO.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <iostream>
#include <vector>
#include <cstring>

_NAMESPACE_BEGIN_

// Phase 3.2 v1 limitations (documented for the on-call reader):
// - The asset's meshes/submeshes are flattened into a single non-indexed
//   GEMesh in source order. We respect the caller's GEMeshDescriptor and
//   write zeros for attributes the source mesh does not provide.
// - Only the base-color material texture is picked up. Normal / metallic
//   / roughness maps land in a follow-up.
// - Tangents, skinning weights, and animations are dropped.
// - Topology is forced to Triangle. MTKMesh emits indexed triangles; we
//   resolve indices into a flat vertex stream so the output matches the
//   triangulation builder's contract (Triangle topology, indexType=None).

namespace {

/// Lowercased file extension (no dot), or "" if none.
std::string lowerExt(const std::string &path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

/// Pull a single per-vertex attribute by semantic name from an MDLMesh.
/// Returns nil if the mesh does not have that attribute.
MDLVertexAttributeData *attrData(MDLMesh *mesh, NSString *name) {
    return [mesh vertexAttributeDataForAttributeNamed:name];
}

/// Read N floats from `attr` at the given vertex index into `out`. Falls
/// back to zeros for any component the source does not carry (e.g. a 2D
/// uv read as a float3 leaves z = 0).
void readFloats(MDLVertexAttributeData *attr, NSUInteger index, int count, float *out) {
    for (int i = 0; i < count; ++i) out[i] = 0.f;
    if (attr == nil || attr.dataStart == nullptr) return;
    const uint8_t *base = (const uint8_t *)attr.dataStart;
    const NSUInteger stride = attr.stride;
    const uint8_t *vert = base + index * stride;

    int srcCount = 0;
    switch (attr.format) {
        case MDLVertexFormatFloat:  srcCount = 1; break;
        case MDLVertexFormatFloat2: srcCount = 2; break;
        case MDLVertexFormatFloat3: srcCount = 3; break;
        case MDLVertexFormatFloat4: srcCount = 4; break;
        default:
            // Other formats (half/uchar/etc.) aren't translated in v1.
            // Caller gets zeros; we log once via the warn-flag in the
            // outer builder.
            return;
    }
    int n = std::min(srcCount, count);
    std::memcpy(out, vert, sizeof(float) * (size_t)n);
}

/// Read one index from a submesh's index buffer. Supports the index
/// formats Model I/O produces (UInt8 / UInt16 / UInt32).
uint32_t readIndex(MDLSubmesh *submesh, NSUInteger i) {
    id<MDLMeshBuffer> ibuf = submesh.indexBuffer;
    const uint8_t *base = (const uint8_t *)[ibuf map].bytes;
    switch (submesh.indexType) {
        case MDLIndexBitDepthUInt8:
            return base[i];
        case MDLIndexBitDepthUInt16:
            return ((const uint16_t *)base)[i];
        case MDLIndexBitDepthUInt32:
            return ((const uint32_t *)base)[i];
        default:
            // Older MDLAssets sometimes report bit depth invalid; fall
            // back to UInt32 which is the most common.
            return ((const uint32_t *)base)[i];
    }
}

/// Resolve the file path of a submesh's base-color texture, or "" if no
/// such texture is present.
std::string baseColorTexturePath(MDLSubmesh *submesh, NSURL *assetURL) {
    MDLMaterial *mat = submesh.material;
    if (mat == nil) return "";
    MDLMaterialProperty *prop = [mat propertyWithSemantic:MDLMaterialSemanticBaseColor];
    if (prop == nil) return "";
    if (prop.type == MDLMaterialPropertyTypeString) {
        // Relative or absolute path string.
        NSString *s = prop.stringValue;
        if (s == nil) return "";
        NSURL *resolved = [NSURL URLWithString:s relativeToURL:assetURL];
        return resolved.path != nil ? std::string(resolved.path.UTF8String) : std::string(s.UTF8String);
    }
    if (prop.type == MDLMaterialPropertyTypeURL) {
        NSURL *u = prop.URLValue;
        return u != nil ? std::string(u.path.UTF8String) : "";
    }
    if (prop.type == MDLMaterialPropertyTypeTexture) {
        // Texture is embedded; v1 doesn't support pulling out raw image
        // bytes via TextureAsset (which is path-based). Skip — caller
        // can attach a texture manually.
        return "";
    }
    return "";
}

class GEMetalMeshAsset : public GEMeshAsset {
    SharedHandle<OmegaGraphicsEngine> engine;
    SharedHandle<GEMesh> loadedMesh;
    std::vector<SharedHandle<GETextureAsset>> loadedTextures;

public:
    explicit GEMetalMeshAsset(SharedHandle<OmegaGraphicsEngine> & e) : engine(e) {}

    bool load(const std::string & path, const LoadOptions & options) override {
        if (engine == nullptr) {
            DEBUG_CRITICAL(DEBUG_DOMAIN_ASSET, "no engine bound.");
            return false;
        }
        if ((options.desiredDescriptor.attributes & GEMeshAttrPosition) == 0) {
            DEBUG_CRITICAL(DEBUG_DOMAIN_ASSET, "desired descriptor must include Position.");
            return false;
        }
        if (options.desiredDescriptor.indexType != GEMeshIndexType::None) {
            DEBUG_CRITICAL(DEBUG_DOMAIN_ASSET, "indexed output not supported in Phase 3 v1.");
            return false;
        }

        const uint32_t attrs = options.desiredDescriptor.attributes;
        const size_t stride = geMeshStrideFor(attrs);
        if (stride == 0) {
            DEBUG_CRITICAL(DEBUG_DOMAIN_ASSET, "empty vertex layout.");
            return false;
        }

        // FBX is the one common format Model I/O cannot load — route it through
        // the shared backend-neutral MeshParser (ufbx), then build the GEMesh
        // from the packed stream exactly as the Model I/O path does below.
        if (lowerExt(path) == "fbx") {
            MeshParser::ParsedMesh parsed;
            if (!MeshParser::parseMesh(path, options.desiredDescriptor, parsed)) {
                DEBUG_CRITICAL(DEBUG_DOMAIN_ASSET, "FBX parse failed: " << path);
                return false;
            }
            return buildFromPacked(parsed.packed, stride, parsed.baseColorTexturePath, options);
        }

        @autoreleasepool {
            id<MTLDevice> device = (__bridge id<MTLDevice>)engine->underlyingNativeDevice();
            if (device == nil) {
                DEBUG_ERROR(DEBUG_DOMAIN_ASSET, "native device is nil.");
                return false;
            }

            NSString *nsPath = [[NSString alloc] initWithUTF8String:path.c_str()];
            NSURL *url = [NSURL fileURLWithPath:nsPath];
            if (![[NSFileManager defaultManager] fileExistsAtPath:nsPath]) {
                DEBUG_CRITICAL(DEBUG_DOMAIN_ASSET, "file not found: " << path);
                return false;
            }

            // No vertex descriptor → MDLAsset preserves the source's
            // native layout. We do our own packing afterwards so the
            // output matches GEMeshDescriptor exactly.
            MDLAsset *asset = [[MDLAsset alloc] initWithURL:url
                                           vertexDescriptor:nil
                                            bufferAllocator:nil];
            if (asset == nil) {
                DEBUG_CRITICAL(DEBUG_DOMAIN_ASSET, "MDLAsset failed to load: " << path);
                return false;
            }

            // Walk every MDLMesh in the asset and accumulate its
            // submeshes' triangles into a single CPU-side packed
            // vertex stream.
            std::vector<float> packed;
            packed.reserve(1024 * (stride / sizeof(float)));

            // Track first base-color texture path encountered, if any.
            std::string baseColorPath;

            bool warnedAttachment = false;

            NSUInteger topCount = [asset count];
            for (NSUInteger objIdx = 0; objIdx < topCount; ++objIdx) {
                MDLObject *obj = [asset objectAtIndex:objIdx];
                MDLMesh *mesh = nil;
                if ([obj isKindOfClass:[MDLMesh class]]) {
                    mesh = (MDLMesh *)obj;
                } else {
                    // Skip non-mesh objects (lights, cameras, etc.).
                    continue;
                }

                MDLVertexAttributeData *aPos = attrData(mesh, MDLVertexAttributePosition);
                MDLVertexAttributeData *aUV  = attrData(mesh, MDLVertexAttributeTextureCoordinate);
                MDLVertexAttributeData *aN   = attrData(mesh, MDLVertexAttributeNormal);
                MDLVertexAttributeData *aC   = attrData(mesh, MDLVertexAttributeColor);

                if (aPos == nil) {
                    DEBUG_INFO(DEBUG_DOMAIN_ASSET, "MDLMesh has no Position; skipped.");
                    continue;
                }
                if ((attrs & (GEMeshAttrUV2 | GEMeshAttrUV3 | GEMeshAttrNormal | GEMeshAttrColor)) != 0
                    && aUV == nil && aN == nil && aC == nil && !warnedAttachment) {
                    DEBUG_INFO(DEBUG_DOMAIN_ASSET, "source mesh missing some "
                               "requested attributes; missing components are written as zeros.");
                    warnedAttachment = true;
                }

                NSArray<MDLSubmesh *> *submeshes = mesh.submeshes;
                for (MDLSubmesh *sub in submeshes) {
                    if (sub.geometryType != MDLGeometryTypeTriangles) {
                        DEBUG_INFO(DEBUG_DOMAIN_ASSET, "non-triangle submesh skipped.");
                        continue;
                    }
                    if (baseColorPath.empty()) {
                        baseColorPath = baseColorTexturePath(sub, url);
                    }

                    NSUInteger indexCount = sub.indexCount;
                    for (NSUInteger i = 0; i < indexCount; ++i) {
                        uint32_t idx = readIndex(sub, i);

                        if (attrs & GEMeshAttrPosition) {
                            float p[3];
                            readFloats(aPos, idx, 3, p);
                            packed.push_back(p[0]);
                            packed.push_back(p[1]);
                            packed.push_back(p[2]);
                        }
                        if (attrs & GEMeshAttrUV2) {
                            float uv[2];
                            readFloats(aUV, idx, 2, uv);
                            packed.push_back(uv[0]);
                            packed.push_back(uv[1]);
                        }
                        if (attrs & GEMeshAttrUV3) {
                            float uv[3];
                            readFloats(aUV, idx, 3, uv);
                            packed.push_back(uv[0]);
                            packed.push_back(uv[1]);
                            packed.push_back(uv[2]);
                        }
                        if (attrs & GEMeshAttrNormal) {
                            float n[3];
                            readFloats(aN, idx, 3, n);
                            packed.push_back(n[0]);
                            packed.push_back(n[1]);
                            packed.push_back(n[2]);
                        }
                        if (attrs & GEMeshAttrColor) {
                            float c[4] = {1.f, 1.f, 1.f, 1.f};
                            // If the source has color, overwrite default white.
                            if (aC != nil) {
                                readFloats(aC, idx, 4, c);
                            }
                            packed.push_back(c[0]);
                            packed.push_back(c[1]);
                            packed.push_back(c[2]);
                            packed.push_back(c[3]);
                        }
                    }
                }
            }

            return buildFromPacked(packed, stride, baseColorPath, options);
        }
    }

    SharedHandle<GEMesh> mesh() const override { return loadedMesh; }

    std::vector<SharedHandle<GETextureAsset>> textureAssets() const override {
        return loadedTextures;
    }

    void release() override {
        loadedMesh.reset();
        loadedTextures.clear();
    }

private:
    /// Build (and store) the GEMesh from an already-packed, GEMeshDescriptor-
    /// ordered vertex stream. Shared by the Model I/O path (OBJ/glTF) and the
    /// MeshParser path (FBX): allocates the GPU vertex buffer, memcpys the
    /// packed floats in, and resolves the base-color texture into the mesh's
    /// bindings. `stride` is in bytes; `packed.size()*sizeof(float)` must be a
    /// whole multiple of it.
    bool buildFromPacked(const std::vector<float> &packed, size_t stride,
                         const std::string &baseColorPath,
                         const LoadOptions &options) {
        const unsigned vertexCount = (unsigned)(packed.size() * sizeof(float) / stride);
        if (vertexCount == 0) {
            DEBUG_ERROR(DEBUG_DOMAIN_ASSET, "no triangles produced.");
            return false;
        }

        // Allocate GPU-visible buffer and copy the packed stream in. We have a
        // packed float array that already matches the buffer layout, so a
        // single memcpy through the MTLBuffer contents is faster and clearer
        // than driving GEBufferWriter attribute-by-attribute.
        BufferDescriptor bdesc;
        bdesc.usage = BufferDescriptor::Upload;
        bdesc.len = (size_t)vertexCount * stride;
        bdesc.objectStride = stride;
        bdesc.opts = Shared;
        SharedHandle<GEBuffer> vbuf = engine->makeBuffer(bdesc);
        if (!vbuf) {
            DEBUG_ERROR(DEBUG_DOMAIN_ASSET, "makeBuffer failed.");
            return false;
        }
        id<MTLBuffer> mtlBuf = (__bridge id<MTLBuffer>)vbuf->native();
        if (mtlBuf == nil) {
            DEBUG_ERROR(DEBUG_DOMAIN_ASSET, "native buffer is nil.");
            return false;
        }
        std::memcpy(mtlBuf.contents, packed.data(), packed.size() * sizeof(float));

        auto m = std::make_shared<GEMesh>();
        m->vertexBuffer = vbuf;
        m->vertexCount = vertexCount;
        m->vertexStride = stride;
        m->descriptor = options.desiredDescriptor;

        // Resolve material textures into TextureAsset and wire into
        // GEMesh.textureBindings.
        if (options.loadMaterialTextures && !baseColorPath.empty()) {
            auto texAsset = GETextureAsset::Create(engine);
            GETextureAsset::LoadOptions topts;
            topts.generateMipmaps = true;
            topts.sRGB = true;
            if (texAsset->load(baseColorPath, topts)) {
                auto tex = texAsset->texture();
                if (tex) {
                    m->textureBindings[options.baseColorSlot] = tex;
                }
                loadedTextures.push_back(texAsset);
            } else {
                DEBUG_INFO(DEBUG_DOMAIN_ASSET, "base-color texture '"
                           << baseColorPath << "' failed to load.");
            }
        }

        loadedMesh = m;
        DEBUG_INFO(DEBUG_DOMAIN_ASSET, "Mesh asset loaded: vertexCount=" << vertexCount);
        return true;
    }
};

}  // namespace

SharedHandle<GEMeshAsset> GEMeshAsset::Create(SharedHandle<OmegaGraphicsEngine> & engine) {
    return SharedHandle<GEMeshAsset>(new GEMetalMeshAsset(engine));
}

_NAMESPACE_END_
