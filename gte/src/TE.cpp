#include "omegaGTE/TE.h"
#include "omegaGTE/GTEMath.h"
// #include "omegaGTE/GTEShaderTypes.h"
#include <array>
#include <optional>
#include <thread>
#include <iostream>
#include <cmath>
#include <cstdint>
#include <exception>
#include <unordered_map>

_NAMESPACE_BEGIN_


struct TETriangulationParams::GraphicsPath3DParams {
    GVectorPath3D * pathes;
    unsigned pathCount;
    float strokeWidth;
};

union TETriangulationParams::Data {

    TriangulationType type;

    GRect rect;

    GRoundedRect rounded_rect;
    
    GCone cone;

    GEllipsoid ellipsoid;

    GRectangularPrism prism;

    GPyramid pyramid;

    GCylinder cylinder;

    GTorus torus;

    GSphere sphere;

    GCapsule capsule;

    GraphicsPath3DParams path3D;

    // Union members include types whose default ctor is non-trivial
    // (e.g. GPoint3D has `float x,y,z = 0;`). C++ implicitly deletes the
    // union's default ctor in that case, which GCC and MSVC `/permissive-`
    // enforce strictly — `new Data{}` would refuse to compile. An empty
    // user-provided default ctor opts out of member initialization; callers
    // always assign a member immediately after construction.
    Data(){}
    ~Data(){
        if(type == TRIANGULATE_RECT){

        }
    };
};


TETriangulationParams::Attachment TETriangulationParams::Attachment::makeColor(const FVec<4> &color) {
    Attachment at{TypeColor};
    at.colorData.color = color;
    return at;
}

TETriangulationParams::Attachment TETriangulationParams::Attachment::makeTexture2D(unsigned width,unsigned height,SharedHandle<GETexture> texture) {
   Attachment at{TypeTexture2D};
   at.colorData.color.~Matrix();
   at.texture2DData.width = width;
   at.texture2DData.height = height;
   at.texture = std::move(texture);
   return at;
}

TETriangulationParams::Attachment TETriangulationParams::Attachment::makeTexture3D(unsigned width,unsigned height,unsigned depth,SharedHandle<GETexture> texture) {
    Attachment at{TypeTexture3D};
   at.colorData.color.~Matrix();
   at.texture3DData.width = width;
   at.texture3DData.height = height;
   at.texture3DData.depth = depth;
   at.texture = std::move(texture);
   return at;
}

TETriangulationParams::Attachment::Attachment(const Attachment & other) : type(other.type), texture(other.texture) {
    switch(type){
        case TypeColor:
            new (&colorData.color) FVec<4>(other.colorData.color);
            break;
        case TypeTexture2D:
            texture2DData = other.texture2DData;
            break;
        case TypeTexture3D:
            texture3DData = other.texture3DData;
            break;
    }
}

TETriangulationParams::Attachment & TETriangulationParams::Attachment::operator=(const Attachment & other) {
    if(this == &other) return *this;
    if(type == TypeColor){
        colorData.color.~Matrix();
    }
    type = other.type;
    texture = other.texture;
    switch(type){
        case TypeColor:
            new (&colorData.color) FVec<4>(other.colorData.color);
            break;
        case TypeTexture2D:
            texture2DData = other.texture2DData;
            break;
        case TypeTexture3D:
            texture3DData = other.texture3DData;
            break;
    }
    return *this;
}

TETriangulationParams::Attachment::~Attachment(){
    if(type == TypeColor){
        colorData.color.~Matrix();
    }
}

void TETriangulationParams::addAttachment(const Attachment &attachment) {
    attachments.push_back(attachment);
}

TETriangulationParams TETriangulationParams::Rect(GRect &rect){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.type = params.params->type = TRIANGULATE_RECT;
    params.params->rect = rect;
    return params;
};

TETriangulationParams TETriangulationParams::RoundedRect(GRoundedRect &roundedRect){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.type = params.params->type = TRIANGULATE_ROUNDEDRECT;
    params.params->rounded_rect = roundedRect;
    return params;
};

TETriangulationParams TETriangulationParams::RectangularPrism(GRectangularPrism &prism){
    TETriangulationParams params;
    params.params.reset(new Data{});
    // params.params.get_deleter().t = params.type;
    params.params->prism = prism;
    params.type = params.params->type = TRIANGULATE_RECTANGULAR_PRISM;
    return params;
};

TETriangulationParams TETriangulationParams::Pyramid(GPyramid &pyramid){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.params->pyramid = pyramid;
    params.type = params.params->type =  TRIANGULATE_PYRAMID;
    return params;
};

TETriangulationParams TETriangulationParams::Ellipsoid(GEllipsoid &ellipsoid){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.params->ellipsoid = ellipsoid;
    params.type = params.params->type =  TRIANGULATE_ELLIPSOID;
    return params;
};

TETriangulationParams TETriangulationParams::Cylinder(GCylinder &cylinder){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.params->cylinder = cylinder;
    params.type = params.params->type =  TRIANGULATE_CYLINDER;
    return params;
};

TETriangulationParams TETriangulationParams::Cone(GCone &cone){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.params->cone = cone;
    params.type = params.params->type =  TRIANGULATE_CONE;
    return params;
};

TETriangulationParams TETriangulationParams::Torus(GTorus &torus){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.params->torus = torus;
    params.type = params.params->type =  TRIANGULATE_TORUS;
    return params;
};

TETriangulationParams TETriangulationParams::Sphere(GSphere &sphere){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.params->sphere = sphere;
    params.type = params.params->type =  TRIANGULATE_SPHERE;
    return params;
};

TETriangulationParams TETriangulationParams::Capsule(GCapsule &capsule){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.params->capsule = capsule;
    params.type = params.params->type =  TRIANGULATE_CAPSULE;
    return params;
};

TETriangulationParams TETriangulationParams::GraphicsPath2D(GVectorPath2D & path,float strokeWidth,bool contour,bool fill,StrokeJoin join,StrokeCap cap){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.graphicsPath2D = std::make_shared<GVectorPath2D>(path);
    params.graphicsPath2DContour = contour;
    params.graphicsPath2DFill = fill;
    params.graphicsPath2DStrokeWidth = strokeWidth;
    params.graphicsPath2DJoin = join;
    params.graphicsPath2DCap = cap;
    params.type = params.params->type = TRIANGULATE_GRAPHICSPATH2D;
    return params;
};

TETriangulationParams TETriangulationParams::GraphicsPath3D(unsigned int vectorPathCount, GVectorPath3D *const vectorPaths,float strokeWidth){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.params->path3D = {vectorPaths,vectorPathCount,strokeWidth};
    params.type = params.params->type =  TRIANGULATE_GRAPHICSPATH3D;
    return params;
};

bool TETriangulationParams::is3DPrimitive() const {
    switch(type){
        case TRIANGULATE_RECTANGULAR_PRISM:
        case TRIANGULATE_PYRAMID:
        case TRIANGULATE_CYLINDER:
        case TRIANGULATE_CONE:
        case TRIANGULATE_TORUS:
        case TRIANGULATE_SPHERE:
        case TRIANGULATE_CAPSULE:
            return true;
        // Rect / RoundedRect / the flat Ellipsoid fan / Path2D / Path3D are not
        // solid 3D primitives; GESpace declines to place them in a 3D space.
        default:
            return false;
    }
}

unsigned int TETriangulationResult::TEMesh::vertexCount() {
    auto polygonCount = vertexPolygons.size();
    return polygonCount * 3;
}

namespace {
    /// Quantized identity of one `TEMesh::Vertex`, used to hash/compare
    /// corners for `buildIndexed()`'s dedup pass. Position components are
    /// quantized to `positionEpsilon`; attachment components (present only
    /// when `hasAttachment`) are quantized to a fixed, finer epsilon — the
    /// dedup goal is collapsing bit-identical shared-vertex data, not fuzzy
    /// attribute matching.
    struct IndexedVertexKey {
        int64_t px = 0, py = 0, pz = 0;
        bool hasAttachment = false;
        int64_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        int64_t u2x = 0, u2y = 0;
        int64_t u3x = 0, u3y = 0, u3z = 0;
        int64_t nx = 0, ny = 0, nz = 0;

        bool operator==(const IndexedVertexKey &o) const {
            return px == o.px && py == o.py && pz == o.pz &&
                   hasAttachment == o.hasAttachment &&
                   c0 == o.c0 && c1 == o.c1 && c2 == o.c2 && c3 == o.c3 &&
                   u2x == o.u2x && u2y == o.u2y &&
                   u3x == o.u3x && u3y == o.u3y && u3z == o.u3z &&
                   nx == o.nx && ny == o.ny && nz == o.nz;
        }
    };

    struct IndexedVertexKeyHash {
        size_t operator()(const IndexedVertexKey &k) const {
            size_t h = 0;
            auto mix = [&h](int64_t v) {
                h ^= std::hash<int64_t>()(v) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
            };
            mix(k.px); mix(k.py); mix(k.pz); mix(k.hasAttachment ? 1 : 0);
            mix(k.c0); mix(k.c1); mix(k.c2); mix(k.c3);
            mix(k.u2x); mix(k.u2y);
            mix(k.u3x); mix(k.u3y); mix(k.u3z);
            mix(k.nx); mix(k.ny); mix(k.nz);
            return h;
        }
    };

    int64_t quantize(float v, float epsilon) {
        return static_cast<int64_t>(std::llround(static_cast<double>(v) / static_cast<double>(epsilon)));
    }
}

void TETriangulationResult::TEMesh::buildIndexed(float positionEpsilon) {
    const float posEps = positionEpsilon > 0.f ? positionEpsilon : 1e-6f;
    const float attachmentEps = 1e-5f;

    IndexedData out;
    std::unordered_map<IndexedVertexKey, uint32_t, IndexedVertexKeyHash> lookup;
    out.vertices.reserve(vertexPolygons.size() * 3);
    out.indices.reserve(vertexPolygons.size() * 3);

    auto emit = [&](const Vertex &v) {
        IndexedVertexKey key;
        key.px = quantize(v.pt.x, posEps);
        key.py = quantize(v.pt.y, posEps);
        key.pz = quantize(v.pt.z, posEps);
        key.hasAttachment = v.attachment.has_value();
        if (key.hasAttachment) {
            const auto &a = *v.attachment;
            key.c0 = quantize(a.color[0][0], attachmentEps);
            key.c1 = quantize(a.color[1][0], attachmentEps);
            key.c2 = quantize(a.color[2][0], attachmentEps);
            key.c3 = quantize(a.color[3][0], attachmentEps);
            key.u2x = quantize(a.texture2Dcoord[0][0], attachmentEps);
            key.u2y = quantize(a.texture2Dcoord[1][0], attachmentEps);
            key.u3x = quantize(a.texture3Dcoord[0][0], attachmentEps);
            key.u3y = quantize(a.texture3Dcoord[1][0], attachmentEps);
            key.u3z = quantize(a.texture3Dcoord[2][0], attachmentEps);
            key.nx = quantize(a.normal[0][0], attachmentEps);
            key.ny = quantize(a.normal[1][0], attachmentEps);
            key.nz = quantize(a.normal[2][0], attachmentEps);
        }

        auto found = lookup.find(key);
        if (found != lookup.end()) {
            out.indices.push_back(found->second);
            return;
        }
        uint32_t newIndex = static_cast<uint32_t>(out.vertices.size());
        out.vertices.push_back(v);
        lookup.emplace(key, newIndex);
        out.indices.push_back(newIndex);
    };

    for (auto &polygon : vertexPolygons) {
        emit(polygon.a);
        emit(polygon.b);
        emit(polygon.c);
    }

    indexedData = std::move(out);
}

unsigned int TETriangulationResult::totalVertexCount() {
    return mesh.vertexCount();
}

TETriangulationParams::~TETriangulationParams() = default;


SharedHandle<OmegaTriangulationEngine> OmegaTriangulationEngine::Create(){
    return std::make_shared<OmegaTriangulationEngine>();
};

GEViewport OmegaTriangulationEngineContext::getEffectiveViewport(){
    return GEViewport{0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
}

void OmegaTriangulationEngineContext::translateCoordsDefaultImpl(float x, float y, float z, GEViewport * viewport, float *x_result, float *y_result, float *z_result){
    // GEViewport adopts the top-left / Y-down convention: y=0 is the top
    // edge of the viewport, y=height is the bottom edge. The mapping to
    // Y-up NDC (Metal / D3D12 native; Vulkan via negative-height viewport)
    // therefore inverts the Y term so y=0 lands at NDC +1.
    //
    // The viewport ORIGIN is subtracted before the divide (Phase 9.3): a
    // viewport anchored at (vp.x, vp.y) must map its own top-left corner to
    // NDC (-1, +1), not the world origin. Without this, any viewport not at
    // (0,0) — precisely what a render pass into a sub-rect configures —
    // produces geometry shifted by the origin term.
    const float width  = viewport->width  != 0.f ? viewport->width  : 1.f;
    const float height = viewport->height != 0.f ? viewport->height : 1.f;

    *x_result = (2.f * (x - viewport->x) / width) - 1.f;
    *y_result = 1.f - (2.f * (y - viewport->y) / height);

    if(z_result != nullptr){
        // Depth maps LINEARLY to [0,1] — the range Vulkan, D3D12 and Metal all
        // clip against. near -> 0, far -> 1.
        //
        // This replaces a three-branch map that was unusable for 3D geometry.
        // It divided z>0 by farDepth and z<0 by *nearDepth*, and every viewport
        // this engine builds has nearDepth = 0 (see makeViewport in each
        // *TEContext) — so every vertex with negative z divided by zero and came
        // out as -infinity, while z==0 fell through a third branch that returned
        // z unchanged. The result was a torn mesh: any primitive straddling z=0
        // (a sphere, torus, capsule, or prism centered on the origin plane) had
        // half its vertices at infinity and a discontinuity across the seam.
        // One continuous affine map has no branches, no seam, and no division by
        // a depth bound that is always zero.
        const float depthRange = viewport->farDepth - viewport->nearDepth;
        *z_result = depthRange != 0.f ? ((z - viewport->nearDepth) / depthRange) : 0.f;
    };
};

// Phase 9.2 — the one place the coordinate space of a triangulation request is
// decided. The CPU path below and every backend's triangulateOnGPU both call
// this, so the GPU can never silently triangulate against a different space
// than the CPU would have.
GEViewport OmegaTriangulationEngineContext::resolveViewport(const TETriangulationParams & params, GEViewport * viewportArg){
    // The coordinate space is a property of the geometry, so a viewport declared
    // on the params is canonical and wins over the loose call argument; the
    // argument is retained for the callers that predate the params field.
    GEViewport fallback = getEffectiveViewport();
    GEViewport resolved = fallback;
    if(params.viewport.has_value()){
        resolved = *params.viewport;
    }
    else if(viewportArg != nullptr){
        resolved = *viewportArg;
    }

    // Local space ignores the viewport entirely, so an unusable one is not worth
    // reporting there.
    if(!params.localSpace && (resolved.width == 0.f || resolved.height == 0.f)){
        std::cerr << "[TE] error: triangulation viewport has zero extent (width="
                  << resolved.width << ", height=" << resolved.height
                  << "); falling back to the effective viewport." << std::endl;
        resolved = fallback;
    }
    return resolved;
}

// The winding counterpart of resolveViewport, and it exists for the same reason:
// the GPU kernels bake a winding decision into their vertex emission order, so
// they must be told the SAME winding the CPU would have normalized to.
GTEPolygonFrontFaceRotation OmegaTriangulationEngineContext::resolveWinding(const TETriangulationParams & params, GTEPolygonFrontFaceRotation frontFaceRotationArg){
    if(params.frontFaceRotation.has_value()){
        return *params.frontFaceRotation;
    }
    return frontFaceRotationArg;
}

inline void OmegaTriangulationEngineContext::_triangulatePriv(const TETriangulationParams &params,GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport,TETriangulationResult & result){
    assert(params.attachments.size() <= 2 && "At most 2 attachments are allowed for each tessellation params");

    GEViewport resolvedViewport = resolveViewport(params, viewport);
    const bool localSpace = params.localSpace;
    viewport = &resolvedViewport;

    // The single coordinate seam every primitive below goes through. Local-space
    // triangulation (Phase 9.6) is an identity pass here rather than a flag
    // threaded into `translateCoords`, so no primitive can forget to honor it:
    // there are 27 call sites and exactly one place that decides what they mean.
    auto mapCoords = [&](float x, float y, float z, GEViewport * vp,
                         float * x_result, float * y_result, float * z_result){
        if(localSpace){
            *x_result = x;
            *y_result = y;
            if(z_result != nullptr) *z_result = z;
            return;
        }
        translateCoords(x, y, z, vp, x_result, y_result, z_result);
    };

    const TETriangulationParams::Attachment * textureAttachment = nullptr;
    const TETriangulationParams::Attachment * colorAttachmentPtr = nullptr;
    if(!params.attachments.empty()){
        auto & a = params.attachments.front();
        if(a.type == TETriangulationParams::Attachment::TypeColor){
            colorAttachmentPtr = &a;
        } else {
            textureAttachment = &a;
        }
    }

    auto makeVec2 = [](float u, float v){
        auto r = FVec<2>::Create();
        r[0][0] = u; r[1][0] = v;
        return r;
    };
    auto makeVec3 = [](float x, float y, float z){
        auto r = FVec<3>::Create();
        r[0][0] = x; r[1][0] = y; r[2][0] = z;
        return r;
    };
    auto makeTex2DAttachment = [&](float u, float v, const FVec<3> & normal){
        return TETriangulationResult::AttachmentData{FVec<4>::Create(), makeVec2(u,v), FVec<3>::Create(), normal};
    };
    auto makeTex3DAttachment = [&](float u, float v, float w, const FVec<3> & normal){
        return TETriangulationResult::AttachmentData{FVec<4>::Create(), FVec<2>::Create(), makeVec3(u,v,w), normal};
    };
    (void)makeTex2DAttachment;
    (void)makeTex3DAttachment;

    // Each triangulation case builds one local mesh and finalizes it through one
    // of the helpers below, which (a) orients triangles per the requested winding
    // mode and (b) stores the single result mesh.
    //
    // Back-face culling is currently off in every consumer, so the winding mode has
    // no visible effect today; it makes the output correct for when culling is on.
    const bool wantCCWWinding = (resolveWinding(params, frontFaceRotation) == GTEPolygonFrontFaceRotation::CounterClockwise);
    auto deviceSignedArea = [](const TETriangulationResult::TEMesh::Polygon & p){
        return (p.b.pt.x - p.a.pt.x) * (p.c.pt.y - p.a.pt.y)
             - (p.b.pt.y - p.a.pt.y) * (p.c.pt.x - p.a.pt.x);
    };
    // Flat, single-sided geometry (coplanar, +Z facing): normalize every triangle
    // to the requested winding so all of them survive back-face culling. Used for
    // shapes whose triangles may be authored with mixed orientation (fans, mixed
    // fill+stroke), where a per-triangle decision is required.
    auto finalizeFlat = [&](TETriangulationResult::TEMesh & m){
        for(auto & p : m.vertexPolygons){
            if((deviceSignedArea(p) > 0.f) != wantCCWWinding){
                std::swap(p.b, p.c);
            }
        }
        result.mesh = std::move(m);
    };
    // Closed/solid geometry: preserve each face's outward orientation (so back-face
    // culling keeps distinguishing front from back) and only reverse the convention
    // when the non-default mode is requested. Calibrated so Clockwise (the default)
    // keeps the authored winding untouched.
    auto finalizeSolid = [&](TETriangulationResult::TEMesh & m){
        if(wantCCWWinding){
            for(auto & p : m.vertexPolygons){
                std::swap(p.b, p.c);
            }
        }
        result.mesh = std::move(m);
    };

    switch(params.type){
        case TETriangulationParams::TRIANGULATE_RECT : {
            std::cout << "Tessalate GRect" << std::endl;
            std::cout << "Viewport: x:" << viewport->x << " y:" << viewport->y << " w:" << viewport->width << " h:" << viewport->height << " " << std::endl;
            GRect &object = params.params->rect;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            TETriangulationResult::TEMesh::Polygon tri {};
            float x0,x1,y0,y1;
            float u,v;
            mapCoords(object.pos.x,object.pos.y,0.f,viewport,&x0,&y0,nullptr);
            mapCoords(object.pos.x + object.w,object.pos.y + object.h,0.f,viewport,&x1,&y1,nullptr);

            std::cout << "X0:" << x0 << ", X1:" << x1 << ", Y0:" << y0 << ", Y1:" << y1 << std::endl;

            tri.a.pt.x = x0;
            tri.a.pt.y = y0;
            tri.b.pt.x = x0;
            tri.b.pt.y = y1;
            tri.c.pt.x = x1;
            tri.c.pt.y = y0;

            std::optional<TETriangulationResult::AttachmentData> extra;

            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETriangulationParams::Attachment::TypeColor){
                    extra = std::make_optional<TETriangulationResult::AttachmentData>({attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
                    tri.a.attachment = tri.b.attachment = tri.c.attachment = extra;
                }
                else if(attachment.type == TETriangulationParams::Attachment::TypeTexture2D){
                    
                    mapCoords(attachment.texture2DData.width,attachment.texture2DData.height,0, viewport,&u,&v,nullptr);
                    auto texCoord = FVec<2>::Create();

                    texCoord[0][0] = 0;
                    texCoord[1][0] = 0;

                    extra = std::make_optional<TETriangulationResult::AttachmentData>({FVec<4>::Create(),texCoord,FVec<3>::Create()});
                    tri.b.attachment = extra;

                    texCoord[1][0] = 1;
                    extra = std::make_optional<TETriangulationResult::AttachmentData>({FVec<4>::Create(),texCoord,FVec<3>::Create()});
                     tri.a.attachment = extra;

                     texCoord[0][0] = 1;
                      texCoord[1][0] = 1;
                    extra = std::make_optional<TETriangulationResult::AttachmentData>({FVec<4>::Create(),texCoord,FVec<3>::Create()});
                     tri.c.attachment = extra;
                }
            }
            

            mesh.vertexPolygons.push_back(tri);

            tri.a.pt.x = x1;
            tri.a.pt.y = y1;

            if(!params.attachments.empty()){
                 auto & attachment = params.attachments.front();
                 if(attachment.type == TETriangulationParams::Attachment::TypeTexture2D){
                     auto texCoord = FVec<2>::Create();
                     texCoord[0][0] = 1;
                     texCoord[1][0] = 0;
                     extra = std::make_optional<TETriangulationResult::AttachmentData>({FVec<4>::Create(),texCoord,FVec<3>::Create()});
                    tri.a.attachment = extra;
                 }
            }

            // T1 was CW (Y-up NDC); the natural T2 vertex order would be CCW.
            // Swap b/c (pt + attachment together) so both triangles share the
            // pipeline's requested front-face winding instead of alternating.
            std::swap(tri.b, tri.c);

            mesh.vertexPolygons.push_back(tri);


            finalizeFlat(mesh);

            break;
        }
        case TETriangulationParams::TRIANGULATE_ROUNDEDRECT : {
            auto & object = params.params->rounded_rect;
            const float ox = object.pos.x;
            const float oy = object.pos.y;
            const float rad_x = std::fmax(0.0f,std::fmin(object.rad_x,object.w * 0.5f));
            const float rad_y = std::fmax(0.0f,std::fmin(object.rad_y,object.h * 0.5f));

            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }

            // The entire rounded rect — center + 4 corner arcs + 4 edge strips —
            // is emitted as a single mesh. See gte/docs/Limitations.rst (Driver
            // Quirks) for why multi-mesh primitives are not allowed.
            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};

            // Tessellate a sub-rect via the regular RECT path into a scratch
            // result, then move its polygons into our shared mesh.
            auto appendRect = [&](const GRect & sub_rect){
                GRect rectCopy = sub_rect;
                auto rect_params = TETriangulationParams::Rect(rectCopy);
                if(colorAttachment){
                    rect_params.addAttachment(
                            TETriangulationParams::Attachment::makeColor(colorAttachment->color));
                }
                TETriangulationResult subResult;
                _triangulatePriv(rect_params, frontFaceRotation, viewport, subResult);
                mesh.vertexPolygons.insert(
                    mesh.vertexPolygons.end(),
                    std::make_move_iterator(subResult.mesh.vertexPolygons.begin()),
                    std::make_move_iterator(subResult.mesh.vertexPolygons.end()));
            };

            auto tessellateArc = [&](GPoint2D start, float ar_x, float ar_y, float angle_start, float angle_end, float _arcStep){
                if(std::fabs(_arcStep) < 0.0001f){
                    return;
                }
                float centerX,centerY;
                mapCoords(start.x,start.y,0.f,viewport,&centerX,&centerY,nullptr);
                GPoint3D pt_a {centerX,centerY,0.f};
                float angle = angle_start;
                while((_arcStep > 0.f) ? (angle < angle_end) : (angle > angle_end)){
                    TETriangulationResult::TEMesh::Polygon p {};

                    float nextAngle = angle + _arcStep;
                    if(_arcStep > 0.f && nextAngle > angle_end){
                        nextAngle = angle_end;
                    }
                    else if(_arcStep < 0.f && nextAngle < angle_end){
                        nextAngle = angle_end;
                    }

                    auto x_f = cosf(angle) * ar_x;
                    auto y_f = sinf(angle) * ar_y;

                    x_f += start.x;
                    y_f += start.y;

                    p.a.pt = pt_a;
                    float x_t,y_t;
                    mapCoords(x_f,y_f,0.f,viewport,&x_t,&y_t,nullptr);
                    p.b.pt = GPoint3D {x_t,y_t,0.f};

                    x_f = cosf(nextAngle) * ar_x;
                    y_f = sinf(nextAngle) * ar_y;

                    x_f += start.x;
                    y_f += start.y;

                    mapCoords(x_f,y_f,0.f,viewport,&x_t,&y_t,nullptr);
                    p.c.pt = GPoint3D {x_t,y_t,0.f};
                    if(colorAttachment){
                        p.a.attachment = p.b.attachment = p.c.attachment = colorAttachment;
                    }

                    mesh.vertexPolygons.push_back(p);
                    angle = nextAngle;
                }
            };

            /// Center
            appendRect(GRect{GPoint2D{ox + rad_x, oy + rad_y},
                             object.w - (2 * rad_x), object.h - (2 * rad_y)});

            /// Bottom Left Arc
            tessellateArc(GPoint2D {ox + rad_x, oy + rad_y}, rad_x, rad_y, float(3.f * PI) / 2.f, PI, -arcStep);

            /// Left Rect
            appendRect(GRect{GPoint2D{ox, oy + rad_y}, rad_x, object.h - (2 * rad_y)});

            /// Top Left Arc
            tessellateArc(GPoint2D {ox + rad_x, oy + object.h - rad_y}, rad_x, rad_y, PI, float(PI) / 2.f, -arcStep);

            /// Top Rect
            appendRect(GRect{GPoint2D{ox + rad_x, oy + object.h - rad_y},
                             object.w - (rad_x * 2), rad_y});

            /// Top Right Arc
            tessellateArc(GPoint2D {ox + object.w - rad_x, oy + object.h - rad_y}, rad_x, rad_y, float(PI) / 2.f, 0, -arcStep);

            /// Right Rect
            appendRect(GRect{GPoint2D{ox + object.w - rad_x, oy + rad_y},
                             rad_x, object.h - (2 * rad_y)});

            /// Bottom Right Arc
            tessellateArc(GPoint2D {ox + object.w - rad_x, oy + rad_y}, rad_x, rad_y, 0, -float(PI) / 2.f, -arcStep);

            /// Bottom Rect
            appendRect(GRect{GPoint2D{ox + rad_x, oy}, object.w - (rad_x * 2), rad_y});

            if(textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D){
                float dx0,dy0,dx1,dy1;
                mapCoords(ox,oy,0.f,viewport,&dx0,&dy0,nullptr);
                mapCoords(ox + object.w,oy + object.h,0.f,viewport,&dx1,&dy1,nullptr);
                const float rangeX = (dx1 - dx0);
                const float rangeY = (dy1 - dy0);
                FVec<3> normal = makeVec3(0.f,0.f,1.f);
                for(auto & poly : mesh.vertexPolygons){
                    auto assign = [&](auto & vert){
                        float u = (std::fabs(rangeX) > 1e-6f) ? ((vert.pt.x - dx0) / rangeX) : 0.f;
                        float v = (std::fabs(rangeY) > 1e-6f) ? ((vert.pt.y - dy0) / rangeY) : 0.f;
                        vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                            makeTex2DAttachment(u,v,normal));
                    };
                    assign(poly.a);
                    assign(poly.b);
                    assign(poly.c);
                }
            }

            if(!mesh.vertexPolygons.empty()){
                finalizeFlat(mesh);
            }

            break;
        }
        case TETriangulationParams::TRIANGULATE_ELLIPSOID : {
            auto & object = params.params->ellipsoid;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }
            const bool hasTex2D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D;
            const bool hasTex3D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture3D;
            const FVec<3> planarNormal = makeVec3(0.f,0.f,1.f);

            float centerX,centerY,centerZ;
            mapCoords(object.x,object.y,object.z,viewport,&centerX,&centerY,&centerZ);

            auto makePoint = [&](float angle){
                float x = object.x + std::cos(angle) * object.rad_x;
                float y = object.y + std::sin(angle) * object.rad_y;
                float tx,ty,tz;
                mapCoords(x,y,object.z,viewport,&tx,&ty,&tz);
                return GPoint3D{tx,ty,tz};
            };

            auto attachmentForAngle = [&](float cosA, float sinA) -> std::optional<TETriangulationResult::AttachmentData> {
                if(hasTex2D){
                    return std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex2DAttachment(0.5f + 0.5f * cosA, 0.5f + 0.5f * sinA, planarNormal));
                }
                if(hasTex3D){
                    return std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex3DAttachment(0.5f + 0.5f * cosA, 0.5f + 0.5f * sinA, 0.5f, planarNormal));
                }
                return colorAttachment;
            };
            auto centerAttachment = [&]() -> std::optional<TETriangulationResult::AttachmentData> {
                if(hasTex2D){
                    return std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex2DAttachment(0.5f, 0.5f, planarNormal));
                }
                if(hasTex3D){
                    return std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex3DAttachment(0.5f, 0.5f, 0.5f, planarNormal));
                }
                return colorAttachment;
            };

            auto center = GPoint3D{centerX,centerY,centerZ};
            const float step = arcStep > 0.f ? arcStep : 0.01f;
            float angle = 0.f;
            auto prev = makePoint(0.f);
            float prevCos = 1.f, prevSin = 0.f;

            while(angle < (2.f * PI)){
                float nextAngle = angle + step;
                if(nextAngle > (2.f * PI)){
                    nextAngle = 2.f * PI;
                }
                auto next = makePoint(nextAngle);
                float nextCos = std::cos(nextAngle);
                float nextSin = std::sin(nextAngle);

                TETriangulationResult::TEMesh::Polygon tri {};
                tri.a.pt = center;
                tri.b.pt = prev;
                tri.c.pt = next;
                tri.a.attachment = centerAttachment();
                tri.b.attachment = attachmentForAngle(prevCos,prevSin);
                tri.c.attachment = attachmentForAngle(nextCos,nextSin);
                mesh.vertexPolygons.push_back(tri);

                prev = next;
                prevCos = nextCos;
                prevSin = nextSin;
                angle = nextAngle;
            }

            finalizeFlat(mesh);
            break;
        }
        case TETriangulationParams::TRIANGULATE_RECTANGULAR_PRISM : {
            auto & object = params.params->prism;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};

            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                    TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }
            const bool hasTex2D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D;
            const bool hasTex3D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture3D;

            float x0,x1,y0,y1,z0,z1;
            mapCoords(object.pos.x,object.pos.y,object.pos.z,viewport,&x0,&y0,&z0);
            mapCoords(object.pos.x + object.w,
                            object.pos.y + object.h,
                            object.pos.z + object.d,
                             viewport,&x1,&y1,&z1);

            struct FaceVert { GPoint3D pt; float u; float v; float ox; float oy; float oz; };
            auto emitFace = [&](const FaceVert corners[4], const FVec<3> & normal){
                const int idx[2][3] = {{0,1,2},{0,2,3}};
                for(int t = 0; t < 2; ++t){
                    TETriangulationResult::TEMesh::Polygon p {};
                    const FaceVert * verts[3] = { &corners[idx[t][0]], &corners[idx[t][1]], &corners[idx[t][2]] };
                    GPoint3D pts[3] = { verts[0]->pt, verts[1]->pt, verts[2]->pt };
                    p.a.pt = pts[0]; p.b.pt = pts[1]; p.c.pt = pts[2];
                    auto setAttachment = [&](auto & vert, const FaceVert & fv){
                        if(hasTex2D){
                            vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                                makeTex2DAttachment(fv.u, fv.v, normal));
                        } else if(hasTex3D){
                            vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                                makeTex3DAttachment(fv.ox, fv.oy, fv.oz, normal));
                        } else if(colorAttachment){
                            auto d = *colorAttachment;
                            d.normal = normal;
                            vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(d);
                        }
                    };
                    setAttachment(p.a, *verts[0]);
                    setAttachment(p.b, *verts[1]);
                    setAttachment(p.c, *verts[2]);
                    mesh.vertexPolygons.push_back(p);
                }
            };

            // Object-space normalized corners for Texture3D mapping.
            const float u0 = 0.f, u1 = 1.f;
            const float v0 = 0.f, v1 = 1.f;

            // Bottom face (y=y0), normal (0,-1,0)
            {
                FaceVert c[4] = {
                    {{x0,y0,z0},u0,v0, 0.f,0.f,0.f},
                    {{x1,y0,z0},u1,v0, 1.f,0.f,0.f},
                    {{x1,y0,z1},u1,v1, 1.f,0.f,1.f},
                    {{x0,y0,z1},u0,v1, 0.f,0.f,1.f},
                };
                emitFace(c, makeVec3(0.f,-1.f,0.f));
            }
            // Front face (z=z0), normal (0,0,-1)
            {
                FaceVert c[4] = {
                    {{x0,y0,z0},u0,v0, 0.f,0.f,0.f},
                    {{x0,y1,z0},u0,v1, 0.f,1.f,0.f},
                    {{x1,y1,z0},u1,v1, 1.f,1.f,0.f},
                    {{x1,y0,z0},u1,v0, 1.f,0.f,0.f},
                };
                emitFace(c, makeVec3(0.f,0.f,-1.f));
            }
            // Left face (x=x1), normal (1,0,0)
            {
                FaceVert c[4] = {
                    {{x1,y0,z0},u0,v0, 1.f,0.f,0.f},
                    {{x1,y1,z0},u0,v1, 1.f,1.f,0.f},
                    {{x1,y1,z1},u1,v1, 1.f,1.f,1.f},
                    {{x1,y0,z1},u1,v0, 1.f,0.f,1.f},
                };
                emitFace(c, makeVec3(1.f,0.f,0.f));
            }
            // Right face (x=x0), normal (-1,0,0)
            {
                FaceVert c[4] = {
                    {{x0,y0,z0},u0,v0, 0.f,0.f,0.f},
                    {{x0,y1,z0},u0,v1, 0.f,1.f,0.f},
                    {{x0,y1,z1},u1,v1, 0.f,1.f,1.f},
                    {{x0,y0,z1},u1,v0, 0.f,0.f,1.f},
                };
                emitFace(c, makeVec3(-1.f,0.f,0.f));
            }
            // Back face (z=z1), normal (0,0,1)
            {
                FaceVert c[4] = {
                    {{x0,y0,z1},u0,v0, 0.f,0.f,1.f},
                    {{x0,y1,z1},u0,v1, 0.f,1.f,1.f},
                    {{x1,y1,z1},u1,v1, 1.f,1.f,1.f},
                    {{x1,y0,z1},u1,v0, 1.f,0.f,1.f},
                };
                emitFace(c, makeVec3(0.f,0.f,1.f));
            }
            // Top face (y=y1), normal (0,1,0)
            {
                FaceVert c[4] = {
                    {{x0,y1,z0},u0,v0, 0.f,1.f,0.f},
                    {{x0,y1,z1},u0,v1, 0.f,1.f,1.f},
                    {{x1,y1,z1},u1,v1, 1.f,1.f,1.f},
                    {{x1,y1,z0},u1,v0, 1.f,1.f,0.f},
                };
                emitFace(c, makeVec3(0.f,1.f,0.f));
            }

            finalizeSolid(mesh);

            break;
        }
        case TETriangulationParams::TRIANGULATE_GRAPHICSPATH2D : {
            if(params.graphicsPath2D == nullptr){
                break;
            }
            auto & path = *params.graphicsPath2D;

            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }
            const bool hasTex2D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D;
            const FVec<3> pathNormal = makeVec3(0.f,0.f,1.f);

            auto toDevicePoint = [&](const GPoint2D &point){
                float x,y;
                mapCoords(point.x,point.y,0.f,viewport,&x,&y,nullptr);
                return GPoint3D{x,y,0.f};
            };

            // Precompute total path length for stroke UV u-coord.
            float totalLen = 0.f;
            if(hasTex2D){
                for(auto it = path.begin(); it != path.end(); it.operator++()){
                    auto seg = *it;
                    if(seg.pt_A == nullptr || seg.pt_B == nullptr) break;
                    const float dx = seg.pt_B->x - seg.pt_A->x;
                    const float dy = seg.pt_B->y - seg.pt_A->y;
                    totalLen += std::sqrt(dx*dx + dy*dy);
                }
                if(params.graphicsPath2DContour && path.size() >= 2){
                    auto last = path.lastPt();
                    auto first = path.firstPt();
                    const float dx = first.x - last.x;
                    const float dy = first.y - last.y;
                    totalLen += std::sqrt(dx*dx + dy*dy);
                }
                if(totalLen <= 0.000001f) totalLen = 1.f;
            }

            // Fill polygons and stroke polygons share a single output mesh so
            // a path always emits one draw. See gte/docs/Limitations.rst
            // (Driver Quirks) for why multi-mesh primitives are not allowed.
            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};

            // Fill (a fan) and stroke (quads + joins + caps) are authored with
            // independent winding, so this mesh is finalized through finalizeFlat,
            // which normalizes every triangle to the requested winding mode.

            // --- Fill ---
            // Fan triangulation from the first vertex using the second attachment as fill color.
            if(params.graphicsPath2DFill && path.size() >= 2){
                std::optional<TETriangulationResult::AttachmentData> fillAttachment;
                bool fillHasTex2D = false;
                if(params.attachments.size() >= 2){
                    auto & att = params.attachments[1];
                    if(att.type == TETriangulationParams::Attachment::TypeColor){
                        fillAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                                TETriangulationResult::AttachmentData{att.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
                    } else if(att.type == TETriangulationParams::Attachment::TypeTexture2D){
                        fillHasTex2D = true;
                    }
                }
                if(fillAttachment || fillHasTex2D){
                    // Collect all points from the path in both object and device space.
                    std::vector<GPoint2D> objPts;
                    std::vector<GPoint3D> pts;
                    objPts.push_back(path.firstPt());
                    pts.push_back(toDevicePoint(path.firstPt()));
                    for(auto it = path.begin(); it != path.end(); it.operator++()){
                        auto seg = *it;
                        if(seg.pt_B == nullptr) break;
                        objPts.push_back(*seg.pt_B);
                        pts.push_back(toDevicePoint(*seg.pt_B));
                    }
                    float minX = objPts[0].x, maxX = objPts[0].x, minY = objPts[0].y, maxY = objPts[0].y;
                    for(auto & p : objPts){
                        if(p.x < minX) minX = p.x; if(p.x > maxX) maxX = p.x;
                        if(p.y < minY) minY = p.y; if(p.y > maxY) maxY = p.y;
                    }
                    const float rangeX = (maxX - minX) > 1e-6f ? (maxX - minX) : 1.f;
                    const float rangeY = (maxY - minY) > 1e-6f ? (maxY - minY) : 1.f;
                    auto makeFillAttachment = [&](size_t idx) -> std::optional<TETriangulationResult::AttachmentData> {
                        if(fillHasTex2D){
                            return std::make_optional<TETriangulationResult::AttachmentData>(
                                makeTex2DAttachment((objPts[idx].x - minX)/rangeX, (objPts[idx].y - minY)/rangeY, pathNormal));
                        }
                        return fillAttachment;
                    };
                    for(size_t i = 1; i + 1 < pts.size(); ++i){
                        TETriangulationResult::TEMesh::Polygon tri {};
                        tri.a.pt = pts[0];
                        tri.b.pt = pts[i];
                        tri.c.pt = pts[i + 1];
                        tri.a.attachment = makeFillAttachment(0);
                        tri.b.attachment = makeFillAttachment(i);
                        tri.c.attachment = makeFillAttachment(i + 1);
                        mesh.vertexPolygons.push_back(tri);
                    }
                }
            }

            // --- Stroke ---
            const float strokeWidth = params.graphicsPath2DStrokeWidth > 0.f ? params.graphicsPath2DStrokeWidth : 0.f;
            if(strokeWidth > 0.f){
                const float halfStroke = strokeWidth * 0.5f;
                const StrokeJoin joinStyle = params.graphicsPath2DJoin;
                const StrokeCap capStyle = params.graphicsPath2DCap;
                const bool closed = params.graphicsPath2DContour;
                const float joinStep = arcStep > 0.f ? arcStep : 0.01f;
                const float miterLimit = 4.f;

                auto strokeAttachment = [&](float u, float v) -> std::optional<TETriangulationResult::AttachmentData> {
                    if(hasTex2D){
                        return std::make_optional<TETriangulationResult::AttachmentData>(
                            makeTex2DAttachment(u, v, pathNormal));
                    }
                    return colorAttachment;
                };
                auto emitTriObj = [&](const GPoint2D & A, float uA, float vA,
                                      const GPoint2D & B, float uB, float vB,
                                      const GPoint2D & C, float uC, float vC){
                    TETriangulationResult::TEMesh::Polygon p {};
                    p.a.pt = toDevicePoint(A); p.a.attachment = strokeAttachment(uA, vA);
                    p.b.pt = toDevicePoint(B); p.b.attachment = strokeAttachment(uB, vB);
                    p.c.pt = toDevicePoint(C); p.c.attachment = strokeAttachment(uC, vC);
                    mesh.vertexPolygons.push_back(p);
                };

                // Collect the ordered polyline points.
                std::vector<GPoint2D> sp;
                sp.push_back(path.firstPt());
                for(auto it = path.begin(); it != path.end(); it.operator++()){
                    auto seg = *it;
                    if(seg.pt_B == nullptr) break;
                    sp.push_back(*seg.pt_B);
                }
                // A closed contour whose last point repeats the first is de-duplicated;
                // the wrap-around segment closes the loop instead.
                if(closed && sp.size() >= 2){
                    const auto & f = sp.front();
                    const auto & l = sp.back();
                    if(std::fabs(f.x - l.x) < 1e-6f && std::fabs(f.y - l.y) < 1e-6f){
                        sp.pop_back();
                    }
                }

                const size_t ptCount = sp.size();
                if(ptCount >= 2){
                    const size_t segCount = closed ? ptCount : ptCount - 1;

                    struct SegGeom { GPoint2D dir; GPoint2D nrm; float len; float cursor0; };
                    std::vector<SegGeom> segs(segCount);
                    float cursor = 0.f;
                    for(size_t i = 0; i < segCount; ++i){
                        const GPoint2D & A = sp[i];
                        const GPoint2D & B = sp[(i + 1) % ptCount];
                        const float dx = B.x - A.x, dy = B.y - A.y;
                        const float len = std::sqrt(dx*dx + dy*dy);
                        GPoint2D dir{0.f,0.f}, nrm{0.f,0.f};
                        if(len > 1e-6f){
                            dir = {dx/len, dy/len};
                            nrm = {-dir.y, dir.x};
                        }
                        segs[i] = {dir, nrm, len, cursor};
                        cursor += len;
                    }

                    auto uAt = [&](float c){ return hasTex2D ? (c / totalLen) : 0.f; };

                    // Segment quads (same winding / UVs as the un-joined stroke).
                    for(size_t i = 0; i < segCount; ++i){
                        const SegGeom & s = segs[i];
                        if(s.len <= 1e-6f) continue;
                        const GPoint2D & A = sp[i];
                        const GPoint2D & B = sp[(i + 1) % ptCount];
                        const float hx = s.nrm.x * halfStroke, hy = s.nrm.y * halfStroke;
                        const GPoint2D a{A.x + hx, A.y + hy};
                        const GPoint2D b{A.x - hx, A.y - hy};
                        const GPoint2D c{B.x + hx, B.y + hy};
                        const GPoint2D d{B.x - hx, B.y - hy};
                        const float u0 = uAt(s.cursor0), u1 = uAt(s.cursor0 + s.len);
                        emitTriObj(a, u0, 0.f, b, u0, 1.f, c, u1, 0.f);
                        emitTriObj(c, u1, 0.f, b, u0, 1.f, d, u1, 1.f);
                    }

                    // Fill the outer wedge between two consecutive segments at vertex V.
                    auto emitJoin = [&](const GPoint2D & V, const SegGeom & s0, const SegGeom & s1, float uVertex){
                        if(s0.len <= 1e-6f || s1.len <= 1e-6f) return;
                        const float cross = s0.dir.x * s1.dir.y - s0.dir.y * s1.dir.x;
                        if(std::fabs(cross) < 1e-6f) return; // collinear: no gap to fill
                        const float side = (cross > 0.f) ? -1.f : 1.f; // outer side faces away from the turn
                        const GPoint2D o0{V.x + side * s0.nrm.x * halfStroke, V.y + side * s0.nrm.y * halfStroke};
                        const GPoint2D o1{V.x + side * s1.nrm.x * halfStroke, V.y + side * s1.nrm.y * halfStroke};
                        const float vOuter = (side > 0.f) ? 0.f : 1.f;

                        StrokeJoin js = joinStyle;
                        GPoint2D miterPt{0.f,0.f};
                        if(js == StrokeJoin::Miter){
                            const float denom = s0.dir.x * s1.dir.y - s0.dir.y * s1.dir.x;
                            bool ok = false;
                            if(std::fabs(denom) > 1e-7f){
                                const float t = ((o1.x - o0.x) * s1.dir.y - (o1.y - o0.y) * s1.dir.x) / denom;
                                miterPt = {o0.x + s0.dir.x * t, o0.y + s0.dir.y * t};
                                const float mdx = miterPt.x - V.x, mdy = miterPt.y - V.y;
                                ok = (std::sqrt(mdx*mdx + mdy*mdy) <= miterLimit * halfStroke);
                            }
                            if(!ok) js = StrokeJoin::Bevel; // clamp past the miter limit
                        }

                        if(js == StrokeJoin::Bevel){
                            emitTriObj(V, uVertex, 0.5f, o0, uVertex, vOuter, o1, uVertex, vOuter);
                        } else if(js == StrokeJoin::Miter){
                            emitTriObj(V, uVertex, 0.5f, o0, uVertex, vOuter, miterPt, uVertex, vOuter);
                            emitTriObj(V, uVertex, 0.5f, miterPt, uVertex, vOuter, o1, uVertex, vOuter);
                        } else { // Round
                            const float a0 = std::atan2(o0.y - V.y, o0.x - V.x);
                            const float a1 = std::atan2(o1.y - V.y, o1.x - V.x);
                            float sweep = a1 - a0;
                            while(sweep > float(PI)) sweep -= 2.f * float(PI);
                            while(sweep < -float(PI)) sweep += 2.f * float(PI);
                            const int steps = std::max(1, (int)std::ceil(std::fabs(sweep) / joinStep));
                            const float da = sweep / float(steps);
                            float ang = a0;
                            GPoint2D prev = o0;
                            for(int k = 0; k < steps; ++k){
                                const float ang2 = ang + da;
                                const GPoint2D cur{V.x + std::cos(ang2) * halfStroke, V.y + std::sin(ang2) * halfStroke};
                                emitTriObj(V, uVertex, 0.5f, prev, uVertex, vOuter, cur, uVertex, vOuter);
                                prev = cur; ang = ang2;
                            }
                        }
                    };

                    if(closed){
                        for(size_t i = 0; i < segCount; ++i){
                            const SegGeom & s0 = segs[(i + segCount - 1) % segCount];
                            const SegGeom & s1 = segs[i];
                            emitJoin(sp[i], s0, s1, uAt(s1.cursor0));
                        }
                    } else {
                        for(size_t i = 1; i < segCount; ++i){
                            emitJoin(sp[i], segs[i-1], segs[i], uAt(segs[i].cursor0));
                        }
                    }

                    // Caps at the endpoints of an open path.
                    if(!closed && capStyle != StrokeCap::Butt){
                        auto emitCap = [&](const GPoint2D & end, const GPoint2D & nrm, const GPoint2D & outward, float uEnd){
                            const GPoint2D pPlus{end.x + nrm.x * halfStroke, end.y + nrm.y * halfStroke};
                            const GPoint2D pMinus{end.x - nrm.x * halfStroke, end.y - nrm.y * halfStroke};
                            if(capStyle == StrokeCap::Square){
                                const GPoint2D qPlus{pPlus.x + outward.x * halfStroke, pPlus.y + outward.y * halfStroke};
                                const GPoint2D qMinus{pMinus.x + outward.x * halfStroke, pMinus.y + outward.y * halfStroke};
                                emitTriObj(pPlus, uEnd, 0.f, qPlus, uEnd, 0.f, pMinus, uEnd, 1.f);
                                emitTriObj(pMinus, uEnd, 1.f, qPlus, uEnd, 0.f, qMinus, uEnd, 1.f);
                            } else { // Round: half-circle fan bulging toward `outward`
                                const float a0 = std::atan2(pPlus.y - end.y, pPlus.x - end.x);
                                float sweep = float(PI);
                                const float testMid = a0 + sweep * 0.5f;
                                if(std::cos(testMid) * outward.x + std::sin(testMid) * outward.y < 0.f){
                                    sweep = -float(PI);
                                }
                                const int steps = std::max(1, (int)std::ceil(std::fabs(sweep) / joinStep));
                                const float da = sweep / float(steps);
                                float ang = a0;
                                GPoint2D prev = pPlus;
                                for(int k = 0; k < steps; ++k){
                                    const float ang2 = ang + da;
                                    const GPoint2D cur{end.x + std::cos(ang2) * halfStroke, end.y + std::sin(ang2) * halfStroke};
                                    emitTriObj(end, uEnd, 0.5f, prev, uEnd, 0.f, cur, uEnd, 1.f);
                                    prev = cur; ang = ang2;
                                }
                            }
                        };
                        const SegGeom & firstSeg = segs.front();
                        if(firstSeg.len > 1e-6f){
                            emitCap(sp.front(), firstSeg.nrm, GPoint2D{-firstSeg.dir.x, -firstSeg.dir.y}, uAt(0.f));
                        }
                        const SegGeom & lastSeg = segs.back();
                        if(lastSeg.len > 1e-6f){
                            emitCap(sp.back(), lastSeg.nrm, GPoint2D{lastSeg.dir.x, lastSeg.dir.y}, uAt(cursor));
                        }
                    }
                }
            }

            if(!mesh.vertexPolygons.empty()){
                finalizeFlat(mesh);
            }
            break;
        }
        case TETriangulationParams::TRIANGULATE_PYRAMID : {
            auto & object = params.params->pyramid;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};

            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }
            const bool hasTex2D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D;
            const bool hasTex3D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture3D;

            float ax,ay,az;
            mapCoords(object.x, object.y + object.h, object.z, viewport, &ax, &ay, &az);
            GPoint3D apex {ax, ay, az};

            float bx0,by0,bz0, bx1,by1,bz1;
            mapCoords(object.x - object.w * 0.5f, object.y, object.z - object.d * 0.5f, viewport, &bx0, &by0, &bz0);
            mapCoords(object.x + object.w * 0.5f, object.y, object.z + object.d * 0.5f, viewport, &bx1, &by1, &bz1);

            GPoint3D b00 {bx0, by0, bz0};
            GPoint3D b10 {bx1, by0, bz0};
            GPoint3D b11 {bx1, by0, bz1};
            GPoint3D b01 {bx0, by0, bz1};

            auto normalize3 = [&](float x, float y, float z){
                float len = std::sqrt(x*x + y*y + z*z);
                if(len > 1e-6f){ x /= len; y /= len; z /= len; }
                return makeVec3(x,y,z);
            };
            // Object-space normalized (for Texture3D): x: 0..1 across w, y: 0..1 bottom->top, z: 0..1 across d.
            auto objN = [&](float ox, float oy, float oz){
                return std::array<float,3>{(ox - (object.x - object.w*0.5f)) / (object.w > 1e-6f ? object.w : 1.f),
                                            (oy - object.y) / (object.h > 1e-6f ? object.h : 1.f),
                                            (oz - (object.z - object.d*0.5f)) / (object.d > 1e-6f ? object.d : 1.f)};
            };
            auto apexObj = objN(object.x, object.y + object.h, object.z);
            auto b00Obj = objN(object.x - object.w*0.5f, object.y, object.z - object.d*0.5f);
            auto b10Obj = objN(object.x + object.w*0.5f, object.y, object.z - object.d*0.5f);
            auto b11Obj = objN(object.x + object.w*0.5f, object.y, object.z + object.d*0.5f);
            auto b01Obj = objN(object.x - object.w*0.5f, object.y, object.z + object.d*0.5f);

            auto setVert = [&](auto & vert, const GPoint3D & pt, float u, float v, const std::array<float,3>& o3, const FVec<3> & normal){
                vert.pt = pt;
                if(hasTex2D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex2DAttachment(u, v, normal));
                } else if(hasTex3D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex3DAttachment(o3[0], o3[1], o3[2], normal));
                } else if(colorAttachment){
                    auto d = *colorAttachment;
                    d.normal = normal;
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(d);
                }
            };

            // Front face (toward -z): apex, b00, b10; normal facing -z roughly (but pyramid has slope).
            {
                FVec<3> n = normalize3(0.f, object.w * 0.5f, -object.h);
                TETriangulationResult::TEMesh::Polygon p {};
                setVert(p.a, apex, 0.5f, 0.f, apexObj, n);
                setVert(p.b, b00, 0.f, 1.f, b00Obj, n);
                setVert(p.c, b10, 1.f, 1.f, b10Obj, n);
                mesh.vertexPolygons.push_back(p);
            }
            // Right face (+x)
            {
                FVec<3> n = normalize3(object.d * 0.5f, object.w * 0.5f, 0.f);
                TETriangulationResult::TEMesh::Polygon p {};
                setVert(p.a, apex, 0.5f, 0.f, apexObj, n);
                setVert(p.b, b10, 0.f, 1.f, b10Obj, n);
                setVert(p.c, b11, 1.f, 1.f, b11Obj, n);
                mesh.vertexPolygons.push_back(p);
            }
            // Back face (+z)
            {
                FVec<3> n = normalize3(0.f, object.w * 0.5f, object.h);
                TETriangulationResult::TEMesh::Polygon p {};
                setVert(p.a, apex, 0.5f, 0.f, apexObj, n);
                setVert(p.b, b11, 0.f, 1.f, b11Obj, n);
                setVert(p.c, b01, 1.f, 1.f, b01Obj, n);
                mesh.vertexPolygons.push_back(p);
            }
            // Left face (-x)
            {
                FVec<3> n = normalize3(-object.d * 0.5f, object.w * 0.5f, 0.f);
                TETriangulationResult::TEMesh::Polygon p {};
                setVert(p.a, apex, 0.5f, 0.f, apexObj, n);
                setVert(p.b, b01, 0.f, 1.f, b01Obj, n);
                setVert(p.c, b00, 1.f, 1.f, b00Obj, n);
                mesh.vertexPolygons.push_back(p);
            }
            // Base (two triangles), normal -Y
            {
                FVec<3> n = makeVec3(0.f,-1.f,0.f);
                TETriangulationResult::TEMesh::Polygon p {};
                setVert(p.a, b00, 0.f, 0.f, b00Obj, n);
                setVert(p.b, b10, 1.f, 0.f, b10Obj, n);
                setVert(p.c, b11, 1.f, 1.f, b11Obj, n);
                mesh.vertexPolygons.push_back(p);
                setVert(p.a, b00, 0.f, 0.f, b00Obj, n);
                setVert(p.b, b11, 1.f, 1.f, b11Obj, n);
                setVert(p.c, b01, 0.f, 1.f, b01Obj, n);
                mesh.vertexPolygons.push_back(p);
            }

            finalizeSolid(mesh);
            break;
        }
        case TETriangulationParams::TRIANGULATE_CYLINDER : {
            auto & object = params.params->cylinder;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }
            const bool hasTex2D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D;
            const bool hasTex3D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture3D;

            float cx_bottom,cy_bottom,cz_bottom, cx_top,cy_top,cz_top;
            mapCoords(object.pos.x, object.pos.y, object.pos.z, viewport, &cx_bottom, &cy_bottom, &cz_bottom);
            mapCoords(object.pos.x, object.pos.y + object.h, object.pos.z, viewport, &cx_top, &cy_top, &cz_top);

            GPoint3D bottomCenter {cx_bottom, cy_bottom, cz_bottom};
            GPoint3D topCenter {cx_top, cy_top, cz_top};

            const float step = arcStep > 0.f ? arcStep : 0.01f;
            float angle = 0.f;

            auto makeRimPoint = [&](float a, float baseY) -> GPoint3D {
                float px = object.pos.x + std::cos(a) * object.r;
                float pz = object.pos.z + std::sin(a) * object.r;
                float tx, ty, tz;
                mapCoords(px, baseY, pz, viewport, &tx, &ty, &tz);
                return GPoint3D{tx, ty, tz};
            };

            auto setVert = [&](auto & vert, const GPoint3D & pt, float u, float v, float ox, float oy, float oz, const FVec<3> & normal){
                vert.pt = pt;
                if(hasTex2D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex2DAttachment(u, v, normal));
                } else if(hasTex3D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex3DAttachment(ox, oy, oz, normal));
                } else if(colorAttachment){
                    auto d = *colorAttachment;
                    d.normal = normal;
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(d);
                }
            };
            const FVec<3> upN = makeVec3(0.f,1.f,0.f);
            const FVec<3> downN = makeVec3(0.f,-1.f,0.f);

            while(angle < 2.f * float(PI)){
                float nextAngle = angle + step;
                if(nextAngle > 2.f * float(PI)) nextAngle = 2.f * float(PI);

                const float cosA = std::cos(angle), sinA = std::sin(angle);
                const float cosN = std::cos(nextAngle), sinN = std::sin(nextAngle);

                GPoint3D bCur = makeRimPoint(angle, object.pos.y);
                GPoint3D bNext = makeRimPoint(nextAngle, object.pos.y);
                GPoint3D tCur = makeRimPoint(angle, object.pos.y + object.h);
                GPoint3D tNext = makeRimPoint(nextAngle, object.pos.y + object.h);

                const float uCur = angle / (2.f * float(PI));
                const float uNext = nextAngle / (2.f * float(PI));

                TETriangulationResult::TEMesh::Polygon p {};

                // Bottom cap (polar UVs)
                setVert(p.a, bottomCenter, 0.5f, 0.5f, 0.5f, 0.f, 0.5f, downN);
                setVert(p.b, bCur, 0.5f + 0.5f*cosA, 0.5f + 0.5f*sinA, 0.5f + 0.5f*cosA, 0.f, 0.5f + 0.5f*sinA, downN);
                setVert(p.c, bNext, 0.5f + 0.5f*cosN, 0.5f + 0.5f*sinN, 0.5f + 0.5f*cosN, 0.f, 0.5f + 0.5f*sinN, downN);
                mesh.vertexPolygons.push_back(p);
                // Top cap
                setVert(p.a, topCenter, 0.5f, 0.5f, 0.5f, 1.f, 0.5f, upN);
                setVert(p.b, tNext, 0.5f + 0.5f*cosN, 0.5f + 0.5f*sinN, 0.5f + 0.5f*cosN, 1.f, 0.5f + 0.5f*sinN, upN);
                setVert(p.c, tCur, 0.5f + 0.5f*cosA, 0.5f + 0.5f*sinA, 0.5f + 0.5f*cosA, 1.f, 0.5f + 0.5f*sinA, upN);
                mesh.vertexPolygons.push_back(p);
                // Barrel quad: radial normals
                FVec<3> nCur = makeVec3(cosA, 0.f, sinA);
                FVec<3> nNext = makeVec3(cosN, 0.f, sinN);
                setVert(p.a, bCur, uCur, 0.f, 0.5f + 0.5f*cosA, 0.f, 0.5f + 0.5f*sinA, nCur);
                setVert(p.b, tCur, uCur, 1.f, 0.5f + 0.5f*cosA, 1.f, 0.5f + 0.5f*sinA, nCur);
                setVert(p.c, tNext, uNext, 1.f, 0.5f + 0.5f*cosN, 1.f, 0.5f + 0.5f*sinN, nNext);
                mesh.vertexPolygons.push_back(p);
                setVert(p.a, bCur, uCur, 0.f, 0.5f + 0.5f*cosA, 0.f, 0.5f + 0.5f*sinA, nCur);
                setVert(p.b, tNext, uNext, 1.f, 0.5f + 0.5f*cosN, 1.f, 0.5f + 0.5f*sinN, nNext);
                setVert(p.c, bNext, uNext, 0.f, 0.5f + 0.5f*cosN, 0.f, 0.5f + 0.5f*sinN, nNext);
                mesh.vertexPolygons.push_back(p);

                angle = nextAngle;
            }

            finalizeSolid(mesh);
            break;
        }
        case TETriangulationParams::TRIANGULATE_CONE : {
            auto & object = params.params->cone;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }
            const bool hasTex2D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D;
            const bool hasTex3D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture3D;

            float apex_tx,apex_ty,apex_tz;
            mapCoords(object.x, object.y + object.h, object.z, viewport, &apex_tx, &apex_ty, &apex_tz);
            GPoint3D apex {apex_tx, apex_ty, apex_tz};

            float base_cx,base_cy,base_cz;
            mapCoords(object.x, object.y, object.z, viewport, &base_cx, &base_cy, &base_cz);
            GPoint3D baseCenter {base_cx, base_cy, base_cz};

            const float step = arcStep > 0.f ? arcStep : 0.01f;
            float angle = 0.f;

            auto makeBasePoint = [&](float a) -> GPoint3D {
                float px = object.x + std::cos(a) * object.r;
                float pz = object.z + std::sin(a) * object.r;
                float tx,ty,tz;
                mapCoords(px, object.y, pz, viewport, &tx, &ty, &tz);
                return GPoint3D{tx,ty,tz};
            };

            auto setVert = [&](auto & vert, const GPoint3D & pt, float u, float v, float ox, float oy, float oz, const FVec<3> & normal){
                vert.pt = pt;
                if(hasTex2D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex2DAttachment(u, v, normal));
                } else if(hasTex3D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex3DAttachment(ox, oy, oz, normal));
                } else if(colorAttachment){
                    auto d = *colorAttachment;
                    d.normal = normal;
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(d);
                }
            };
            const FVec<3> downN = makeVec3(0.f,-1.f,0.f);
            const float invTwoPi = 1.f / (2.f * float(PI));

            while(angle < 2.f * float(PI)){
                float nextAngle = angle + step;
                if(nextAngle > 2.f * float(PI)) nextAngle = 2.f * float(PI);

                const float cosA = std::cos(angle), sinA = std::sin(angle);
                const float cosN = std::cos(nextAngle), sinN = std::sin(nextAngle);

                GPoint3D cur = makeBasePoint(angle);
                GPoint3D next = makeBasePoint(nextAngle);

                // Side triangle normal: slanted outward normal, approximate as radial in horizontal plane.
                FVec<3> sideNCur = makeVec3(cosA, object.r > 1e-6f ? (object.r / object.h) : 0.f, sinA);
                FVec<3> sideNNext = makeVec3(cosN, object.r > 1e-6f ? (object.r / object.h) : 0.f, sinN);
                FVec<3> apexN = makeVec3(0.f,1.f,0.f);

                TETriangulationResult::TEMesh::Polygon p {};
                // Side
                setVert(p.a, apex, 0.5f*(angle + nextAngle)*invTwoPi, 0.f, 0.5f, 1.f, 0.5f, apexN);
                setVert(p.b, cur, angle*invTwoPi, 1.f, 0.5f + 0.5f*cosA, 0.f, 0.5f + 0.5f*sinA, sideNCur);
                setVert(p.c, next, nextAngle*invTwoPi, 1.f, 0.5f + 0.5f*cosN, 0.f, 0.5f + 0.5f*sinN, sideNNext);
                mesh.vertexPolygons.push_back(p);
                // Base
                setVert(p.a, baseCenter, 0.5f, 0.5f, 0.5f, 0.f, 0.5f, downN);
                setVert(p.b, next, 0.5f + 0.5f*cosN, 0.5f + 0.5f*sinN, 0.5f + 0.5f*cosN, 0.f, 0.5f + 0.5f*sinN, downN);
                setVert(p.c, cur, 0.5f + 0.5f*cosA, 0.5f + 0.5f*sinA, 0.5f + 0.5f*cosA, 0.f, 0.5f + 0.5f*sinA, downN);
                mesh.vertexPolygons.push_back(p);

                angle = nextAngle;
            }

            finalizeSolid(mesh);
            break;
        }
        case TETriangulationParams::TRIANGULATE_TORUS : {
            auto & object = params.params->torus;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }
            const bool hasTex2D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D;
            const bool hasTex3D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture3D;

            const float R = object.majorRadius;
            const float r = object.minorRadius;
            const float step = arcStep > 0.f ? arcStep : 0.01f;
            const float twoPi = 2.f * float(PI);

            // Y-up: symmetry axis is Y, major circle lies in the XZ plane
            // (matching cylinder/cone), tube cross-section bulges along Y.
            // theta sweeps the major circle, phi sweeps the tube cross-section.
            auto makePoint = [&](float theta, float phi) -> GPoint3D {
                float ringR = R + r * std::cos(phi);
                float wx = object.center.x + ringR * std::cos(theta);
                float wy = object.center.y + r * std::sin(phi);
                float wz = object.center.z + ringR * std::sin(theta);
                float tx,ty,tz;
                mapCoords(wx,wy,wz,viewport,&tx,&ty,&tz);
                return GPoint3D{tx,ty,tz};
            };
            auto makeNormal = [&](float theta, float phi) -> FVec<3> {
                return makeVec3(std::cos(phi)*std::cos(theta), std::sin(phi), std::cos(phi)*std::sin(theta));
            };
            auto setVert = [&](auto & vert, const GPoint3D & pt, float u, float v, const FVec<3> & normal){
                vert.pt = pt;
                if(hasTex2D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex2DAttachment(u, v, normal));
                } else if(hasTex3D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex3DAttachment(0.5f + 0.5f*normal[0][0], 0.5f + 0.5f*normal[1][0], 0.5f + 0.5f*normal[2][0], normal));
                } else if(colorAttachment){
                    auto d = *colorAttachment;
                    d.normal = normal;
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(d);
                }
            };

            float theta = 0.f;
            while(theta < twoPi){
                float thetaNext = theta + step;
                if(thetaNext > twoPi) thetaNext = twoPi;
                const float u0 = theta / twoPi, u1 = thetaNext / twoPi;

                float phi = 0.f;
                while(phi < twoPi){
                    float phiNext = phi + step;
                    if(phiNext > twoPi) phiNext = twoPi;
                    const float v0 = phi / twoPi, v1 = phiNext / twoPi;

                    GPoint3D p00 = makePoint(theta, phi);
                    GPoint3D p01 = makePoint(theta, phiNext);
                    GPoint3D p10 = makePoint(thetaNext, phi);
                    GPoint3D p11 = makePoint(thetaNext, phiNext);
                    FVec<3> n00 = makeNormal(theta, phi);
                    FVec<3> n01 = makeNormal(theta, phiNext);
                    FVec<3> n10 = makeNormal(thetaNext, phi);
                    FVec<3> n11 = makeNormal(thetaNext, phiNext);

                    // Winding flipped vs. the plan's Z-axis form: the Y<->Z
                    // swap reflects the parametrization, so emit (a,c,b) order
                    // to keep front faces outward like the other primitives.
                    TETriangulationResult::TEMesh::Polygon p {};
                    setVert(p.a, p00, u0, v0, n00);
                    setVert(p.b, p11, u1, v1, n11);
                    setVert(p.c, p10, u1, v0, n10);
                    mesh.vertexPolygons.push_back(p);
                    setVert(p.a, p00, u0, v0, n00);
                    setVert(p.b, p01, u0, v1, n01);
                    setVert(p.c, p11, u1, v1, n11);
                    mesh.vertexPolygons.push_back(p);

                    phi = phiNext;
                }
                theta = thetaNext;
            }

            finalizeSolid(mesh);
            break;
        }
        case TETriangulationParams::TRIANGULATE_SPHERE : {
            auto & object = params.params->sphere;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }
            const bool hasTex2D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D;
            const bool hasTex3D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture3D;

            const float rad = object.radius;
            const float step = arcStep > 0.f ? arcStep : 0.01f;
            const float twoPi = 2.f * float(PI);
            const float onePi = float(PI);

            // theta = latitude (0 at +Y north pole .. PI at -Y south pole), phi = longitude.
            auto makePoint = [&](float thetaA, float phiA) -> GPoint3D {
                float ringR = rad * std::sin(thetaA);
                float wx = object.center.x + ringR * std::cos(phiA);
                float wy = object.center.y + rad * std::cos(thetaA);
                float wz = object.center.z + ringR * std::sin(phiA);
                float tx,ty,tz;
                mapCoords(wx,wy,wz,viewport,&tx,&ty,&tz);
                return GPoint3D{tx,ty,tz};
            };
            auto makeNormal = [&](float thetaA, float phiA) -> FVec<3> {
                return makeVec3(std::sin(thetaA)*std::cos(phiA), std::cos(thetaA), std::sin(thetaA)*std::sin(phiA));
            };
            auto setVert = [&](auto & vert, const GPoint3D & pt, float u, float v, const FVec<3> & normal){
                vert.pt = pt;
                if(hasTex2D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex2DAttachment(u, v, normal));
                } else if(hasTex3D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex3DAttachment(0.5f + 0.5f*normal[0][0], 0.5f + 0.5f*normal[1][0], 0.5f + 0.5f*normal[2][0], normal));
                } else if(colorAttachment){
                    auto d = *colorAttachment;
                    d.normal = normal;
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(d);
                }
            };

            float theta = 0.f;
            while(theta < onePi){
                float thetaNext = theta + step;
                if(thetaNext > onePi) thetaNext = onePi;
                const bool northPole = (theta <= 1e-6f);
                const bool southPole = (thetaNext >= onePi - 1e-6f);
                const float v0 = theta / onePi, v1 = thetaNext / onePi;

                float phi = 0.f;
                while(phi < twoPi){
                    float phiNext = phi + step;
                    if(phiNext > twoPi) phiNext = twoPi;
                    const float u0 = phi / twoPi, u1 = phiNext / twoPi;
                    const float uMid = 0.5f * (phi + phiNext) / twoPi;
                    const float phiMid = 0.5f * (phi + phiNext);

                    TETriangulationResult::TEMesh::Polygon p {};
                    if(northPole){
                        setVert(p.a, makePoint(theta, phiMid), uMid, v0, makeNormal(theta, phiMid));
                        setVert(p.b, makePoint(thetaNext, phi), u0, v1, makeNormal(thetaNext, phi));
                        setVert(p.c, makePoint(thetaNext, phiNext), u1, v1, makeNormal(thetaNext, phiNext));
                        mesh.vertexPolygons.push_back(p);
                    } else if(southPole){
                        setVert(p.a, makePoint(theta, phi), u0, v0, makeNormal(theta, phi));
                        setVert(p.b, makePoint(thetaNext, phiMid), uMid, v1, makeNormal(thetaNext, phiMid));
                        setVert(p.c, makePoint(theta, phiNext), u1, v0, makeNormal(theta, phiNext));
                        mesh.vertexPolygons.push_back(p);
                    } else {
                        GPoint3D p00 = makePoint(theta, phi);
                        GPoint3D p01 = makePoint(theta, phiNext);
                        GPoint3D p10 = makePoint(thetaNext, phi);
                        GPoint3D p11 = makePoint(thetaNext, phiNext);
                        FVec<3> n00 = makeNormal(theta, phi);
                        FVec<3> n01 = makeNormal(theta, phiNext);
                        FVec<3> n10 = makeNormal(thetaNext, phi);
                        FVec<3> n11 = makeNormal(thetaNext, phiNext);
                        setVert(p.a, p00, u0, v0, n00);
                        setVert(p.b, p10, u0, v1, n10);
                        setVert(p.c, p11, u1, v1, n11);
                        mesh.vertexPolygons.push_back(p);
                        setVert(p.a, p00, u0, v0, n00);
                        setVert(p.b, p11, u1, v1, n11);
                        setVert(p.c, p01, u1, v0, n01);
                        mesh.vertexPolygons.push_back(p);
                    }
                    phi = phiNext;
                }
                theta = thetaNext;
            }

            finalizeSolid(mesh);
            break;
        }
        case TETriangulationParams::TRIANGULATE_CAPSULE : {
            auto & object = params.params->capsule;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }
            const bool hasTex2D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D;
            const bool hasTex3D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture3D;

            const float r = object.radius;
            const float step = arcStep > 0.f ? arcStep : 0.01f;
            const float twoPi = 2.f * float(PI);
            const float onePi = float(PI);
            const float halfPi = 0.5f * float(PI);
            const float bottomY = object.pos.y;            // bottom hemisphere center
            const float topY = object.pos.y + object.height; // top hemisphere center
            const float totalH = object.height + 2.f * r;   // south pole .. north pole
            const float vBase = object.pos.y - r;           // world Y of south pole

            auto makeNormal = [&](float thetaA, float phiA) -> FVec<3> {
                return makeVec3(std::sin(thetaA)*std::cos(phiA), std::cos(thetaA), std::sin(thetaA)*std::sin(phiA));
            };
            auto setVert = [&](auto & vert, const GPoint3D & pt, float u, float v, const FVec<3> & normal){
                vert.pt = pt;
                if(hasTex2D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex2DAttachment(u, v, normal));
                } else if(hasTex3D){
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        makeTex3DAttachment(0.5f + 0.5f*normal[0][0], 0.5f + 0.5f*normal[1][0], 0.5f + 0.5f*normal[2][0], normal));
                } else if(colorAttachment){
                    auto d = *colorAttachment;
                    d.normal = normal;
                    vert.attachment = std::make_optional<TETriangulationResult::AttachmentData>(d);
                }
            };
            auto vForY = [&](float wy){ return totalH > 1e-6f ? (wy - vBase) / totalH : 0.f; };

            // Hemisphere centered at centerY, sweeping theta in [thetaLo, thetaHi].
            auto makeHemiPoint = [&](float centerY, float thetaA, float phiA) -> GPoint3D {
                float ringR = r * std::sin(thetaA);
                float wx = object.pos.x + ringR * std::cos(phiA);
                float wy = centerY + r * std::cos(thetaA);
                float wz = object.pos.z + ringR * std::sin(phiA);
                float tx,ty,tz;
                mapCoords(wx,wy,wz,viewport,&tx,&ty,&tz);
                return GPoint3D{tx,ty,tz};
            };
            auto hemiV = [&](float centerY, float thetaA){ return vForY(centerY + r * std::cos(thetaA)); };

            auto emitHemisphere = [&](float centerY, float thetaLo, float thetaHi){
                float theta = thetaLo;
                while(theta < thetaHi){
                    float thetaNext = theta + step;
                    if(thetaNext > thetaHi) thetaNext = thetaHi;
                    const bool northPole = (theta <= 1e-6f);
                    const bool southPole = (thetaNext >= onePi - 1e-6f);
                    const float vCur = hemiV(centerY, theta);
                    const float vNext = hemiV(centerY, thetaNext);

                    float phi = 0.f;
                    while(phi < twoPi){
                        float phiNext = phi + step;
                        if(phiNext > twoPi) phiNext = twoPi;
                        const float u0 = phi / twoPi, u1 = phiNext / twoPi;
                        const float uMid = 0.5f * (phi + phiNext) / twoPi;
                        const float phiMid = 0.5f * (phi + phiNext);

                        TETriangulationResult::TEMesh::Polygon p {};
                        if(northPole){
                            setVert(p.a, makeHemiPoint(centerY, theta, phiMid), uMid, vCur, makeNormal(theta, phiMid));
                            setVert(p.b, makeHemiPoint(centerY, thetaNext, phi), u0, vNext, makeNormal(thetaNext, phi));
                            setVert(p.c, makeHemiPoint(centerY, thetaNext, phiNext), u1, vNext, makeNormal(thetaNext, phiNext));
                            mesh.vertexPolygons.push_back(p);
                        } else if(southPole){
                            setVert(p.a, makeHemiPoint(centerY, theta, phi), u0, vCur, makeNormal(theta, phi));
                            setVert(p.b, makeHemiPoint(centerY, thetaNext, phiMid), uMid, vNext, makeNormal(thetaNext, phiMid));
                            setVert(p.c, makeHemiPoint(centerY, theta, phiNext), u1, vCur, makeNormal(theta, phiNext));
                            mesh.vertexPolygons.push_back(p);
                        } else {
                            GPoint3D p00 = makeHemiPoint(centerY, theta, phi);
                            GPoint3D p01 = makeHemiPoint(centerY, theta, phiNext);
                            GPoint3D p10 = makeHemiPoint(centerY, thetaNext, phi);
                            GPoint3D p11 = makeHemiPoint(centerY, thetaNext, phiNext);
                            FVec<3> n00 = makeNormal(theta, phi);
                            FVec<3> n01 = makeNormal(theta, phiNext);
                            FVec<3> n10 = makeNormal(thetaNext, phi);
                            FVec<3> n11 = makeNormal(thetaNext, phiNext);
                            setVert(p.a, p00, u0, vCur, n00);
                            setVert(p.b, p10, u0, vNext, n10);
                            setVert(p.c, p11, u1, vNext, n11);
                            mesh.vertexPolygons.push_back(p);
                            setVert(p.a, p00, u0, vCur, n00);
                            setVert(p.b, p11, u1, vNext, n11);
                            setVert(p.c, p01, u1, vCur, n01);
                            mesh.vertexPolygons.push_back(p);
                        }
                        phi = phiNext;
                    }
                    theta = thetaNext;
                }
            };

            // Top hemisphere: north pole (+Y) down to equator.
            emitHemisphere(topY, 0.f, halfPi);
            // Bottom hemisphere: equator down to south pole (-Y).
            emitHemisphere(bottomY, halfPi, onePi);

            // Barrel between the two equators (radius r, axis along Y).
            auto makeBarrelPoint = [&](float phiA, float wy) -> GPoint3D {
                float wx = object.pos.x + r * std::cos(phiA);
                float wz = object.pos.z + r * std::sin(phiA);
                float tx,ty,tz;
                mapCoords(wx,wy,wz,viewport,&tx,&ty,&tz);
                return GPoint3D{tx,ty,tz};
            };
            const float vBottom = vForY(bottomY);
            const float vTop = vForY(topY);
            float phi = 0.f;
            while(phi < twoPi){
                float phiNext = phi + step;
                if(phiNext > twoPi) phiNext = twoPi;
                const float cosA = std::cos(phi), sinA = std::sin(phi);
                const float cosN = std::cos(phiNext), sinN = std::sin(phiNext);
                const float u0 = phi / twoPi, u1 = phiNext / twoPi;

                GPoint3D bCur = makeBarrelPoint(phi, bottomY);
                GPoint3D bNext = makeBarrelPoint(phiNext, bottomY);
                GPoint3D tCur = makeBarrelPoint(phi, topY);
                GPoint3D tNext = makeBarrelPoint(phiNext, topY);
                FVec<3> nCur = makeVec3(cosA, 0.f, sinA);
                FVec<3> nNext = makeVec3(cosN, 0.f, sinN);

                TETriangulationResult::TEMesh::Polygon p {};
                setVert(p.a, bCur, u0, vBottom, nCur);
                setVert(p.b, tCur, u0, vTop, nCur);
                setVert(p.c, tNext, u1, vTop, nNext);
                mesh.vertexPolygons.push_back(p);
                setVert(p.a, bCur, u0, vBottom, nCur);
                setVert(p.b, tNext, u1, vTop, nNext);
                setVert(p.c, bNext, u1, vBottom, nNext);
                mesh.vertexPolygons.push_back(p);

                phi = phiNext;
            }

            finalizeSolid(mesh);
            break;
        }
        case TETriangulationParams::TRIANGULATE_GRAPHICSPATH3D : {
            auto & path3DParams = params.params->path3D;
            if(path3DParams.pathes == nullptr || path3DParams.pathCount == 0){
                break;
            }

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }
            const bool hasTex2D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D;
            const bool hasTex3D = textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture3D;

            // Pre-compute total length per path for UV parameterization.
            std::vector<float> pathLengths(path3DParams.pathCount, 0.f);
            if(hasTex2D || hasTex3D){
                for(unsigned pi = 0; pi < path3DParams.pathCount; ++pi){
                    auto & path = path3DParams.pathes[pi];
                    float totLen = 0.f;
                    for(auto it = path.begin(); it != path.end(); it.operator++()){
                        auto seg = *it;
                        if(seg.pt_A == nullptr || seg.pt_B == nullptr) break;
                        float dx = seg.pt_B->x - seg.pt_A->x;
                        float dy = seg.pt_B->y - seg.pt_A->y;
                        float dz = seg.pt_B->z - seg.pt_A->z;
                        totLen += std::sqrt(dx*dx + dy*dy + dz*dz);
                    }
                    pathLengths[pi] = totLen > 1e-6f ? totLen : 1.f;
                }
            }

            const float strokeWidth = path3DParams.strokeWidth > 0.f ? path3DParams.strokeWidth : 1.f;
            const float halfStroke = strokeWidth * 0.5f;

            auto toDevice3D = [&](const GPoint3D & point) -> GPoint3D {
                float tx,ty,tz;
                mapCoords(point.x, point.y, point.z, viewport, &tx, &ty, &tz);
                return GPoint3D{tx,ty,tz};
            };

            for(unsigned pi = 0; pi < path3DParams.pathCount; ++pi){
                auto & path = path3DParams.pathes[pi];
                float cursor = 0.f;
                const float totalLen = pathLengths[pi];

                for(auto it = path.begin(); it != path.end(); it.operator++()){
                    auto seg = *it;
                    if(seg.pt_A == nullptr || seg.pt_B == nullptr) break;

                    const GPoint3D & A = *seg.pt_A;
                    const GPoint3D & B = *seg.pt_B;

                    float dx = B.x - A.x;
                    float dy = B.y - A.y;
                    float dz = B.z - A.z;
                    float len = std::sqrt(dx*dx + dy*dy + dz*dz);
                    if(len <= 0.000001f) continue;

                    dx /= len; dy /= len; dz /= len;

                    // Build a perpendicular vector via cross product with a non-parallel axis
                    float ux, uy, uz;
                    if(std::fabs(dx) < 0.9f){
                        ux = 0.f; uy = -dz; uz = dy;
                    } else {
                        ux = dz; uy = 0.f; uz = -dx;
                    }
                    float ulen = std::sqrt(ux*ux + uy*uy + uz*uz);
                    if(ulen > 0.000001f){ ux /= ulen; uy /= ulen; uz /= ulen; }

                    float nx = ux * halfStroke;
                    float ny = uy * halfStroke;
                    float nz = uz * halfStroke;

                    GPoint3D a0 = toDevice3D({A.x + nx, A.y + ny, A.z + nz});
                    GPoint3D a1 = toDevice3D({A.x - nx, A.y - ny, A.z - nz});
                    GPoint3D b0 = toDevice3D({B.x + nx, B.y + ny, B.z + nz});
                    GPoint3D b1 = toDevice3D({B.x - nx, B.y - ny, B.z - nz});

                    const FVec<3> segNormal = makeVec3(ux, uy, uz);
                    const float uStart = (hasTex2D || hasTex3D) ? (cursor / totalLen) : 0.f;
                    const float uEnd = (hasTex2D || hasTex3D) ? ((cursor + len) / totalLen) : 0.f;
                    cursor += len;

                    auto makeAttachment = [&](float u, float v) -> std::optional<TETriangulationResult::AttachmentData> {
                        if(hasTex2D){
                            return std::make_optional<TETriangulationResult::AttachmentData>(
                                makeTex2DAttachment(u, v, segNormal));
                        }
                        if(hasTex3D){
                            return std::make_optional<TETriangulationResult::AttachmentData>(
                                makeTex3DAttachment(u, v, 0.f, segNormal));
                        }
                        if(colorAttachment){
                            auto d = *colorAttachment;
                            d.normal = segNormal;
                            return std::make_optional<TETriangulationResult::AttachmentData>(d);
                        }
                        return std::nullopt;
                    };

                    TETriangulationResult::TEMesh::Polygon p {};
                    p.a.pt = a0; p.b.pt = a1; p.c.pt = b0;
                    p.a.attachment = makeAttachment(uStart, 0.f);
                    p.b.attachment = makeAttachment(uStart, 1.f);
                    p.c.attachment = makeAttachment(uEnd, 0.f);
                    mesh.vertexPolygons.push_back(p);
                    p.a.pt = b0; p.b.pt = a1; p.c.pt = b1;
                    p.a.attachment = makeAttachment(uEnd, 0.f);
                    p.b.attachment = makeAttachment(uStart, 1.f);
                    p.c.attachment = makeAttachment(uEnd, 1.f);
                    mesh.vertexPolygons.push_back(p);
                }
            }

            if(!mesh.vertexPolygons.empty()){
                finalizeSolid(mesh);
            }
            break;
        }
        default: {
            break;
        }
    }

};

void OmegaTriangulationEngineContext::extractGPUTriangulationParams(const TETriangulationParams &params, GPUTriangulationExtractedParams &out) {
    out = {};
    if (!params.attachments.empty()) {
        auto &att = params.attachments.front();
        if (att.type == TETriangulationParams::Attachment::TypeColor) {
            out.hasColor = true;
            FVec<4> c = att.colorData.color;
            out.cr = c[0][0];
            out.cg = c[1][0];
            out.cb = c[2][0];
            out.ca = c[3][0];
        }
    }
    switch (params.type) {
        case TETriangulationParams::TRIANGULATE_RECT: {
            out.type = GPUTriangulationExtractedParams::Rect;
            out.rx = params.params->rect.pos.x;
            out.ry = params.params->rect.pos.y;
            out.rw = params.params->rect.w;
            out.rh = params.params->rect.h;
            break;
        }
        case TETriangulationParams::TRIANGULATE_ROUNDEDRECT:
            out.type = GPUTriangulationExtractedParams::RoundedRect;
            break;
        case TETriangulationParams::TRIANGULATE_ELLIPSOID: {
            out.type = GPUTriangulationExtractedParams::Ellipsoid;
            out.ex = params.params->ellipsoid.x;
            out.ey = params.params->ellipsoid.y;
            out.erad_x = params.params->ellipsoid.rad_x;
            out.erad_y = params.params->ellipsoid.rad_y;
            break;
        }
        case TETriangulationParams::TRIANGULATE_RECTANGULAR_PRISM: {
            out.type = GPUTriangulationExtractedParams::RectPrism;
            out.px = params.params->prism.pos.x;
            out.py = params.params->prism.pos.y;
            out.pz = params.params->prism.pos.z;
            out.pw = params.params->prism.w;
            out.ph = params.params->prism.h;
            out.pd = params.params->prism.d;
            break;
        }
        case TETriangulationParams::TRIANGULATE_GRAPHICSPATH2D: {
            out.type = GPUTriangulationExtractedParams::Path2D;
            out.strokeWidth = params.graphicsPath2DStrokeWidth;
            out.contour = params.graphicsPath2DContour;
            if (params.graphicsPath2D) {
                auto &path = *params.graphicsPath2D;
                for (auto it = path.begin(); it != path.end(); it.operator++()) {
                    auto seg = *it;
                    if (!seg.pt_A || !seg.pt_B) break;
                    out.pathSegments.push_back({seg.pt_A->x, seg.pt_A->y, seg.pt_B->x, seg.pt_B->y});
                }
                if (out.contour && path.size() >= 2) {
                    auto last = path.lastPt();
                    auto first = path.firstPt();
                    out.pathSegments.push_back({last.x, last.y, first.x, first.y});
                }
            }
            break;
        }
        default:
            out.type = GPUTriangulationExtractedParams::Other;
            break;
    }
}

SharedHandle<OmegaTriangulationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> & renderTarget);
SharedHandle<OmegaTriangulationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> & renderTarget,
                                                                                  SharedHandle<GECommandQueue> & queue);

SharedHandle<OmegaTriangulationEngineContext> OmegaTriangulationEngine::createTEContextFromNativeRenderTarget(SharedHandle<GENativeRenderTarget> & renderTarget){
    return CreateNativeRenderTargetTEContext(renderTarget);
};

SharedHandle<OmegaTriangulationEngineContext> OmegaTriangulationEngine::createTEContextFromTextureRenderTarget(SharedHandle<GETextureRenderTarget> & renderTarget,
                                                                                                                SharedHandle<GECommandQueue> & queue){
    return CreateTextureRenderTargetTEContext(renderTarget, queue);
};

std::future<TETriangulationResult> OmegaTriangulationEngineContext::triangulateAsync(const TETriangulationParams &params,GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport){
    std::promise<TETriangulationResult> prom;
    auto fut = prom.get_future();
    TETriangulationParams paramsCopy = params;
    std::optional<GEViewport> viewportCopy = std::nullopt;
    if(viewport != nullptr){
        viewportCopy = *viewport;
    }
    std::thread worker([this, promise = std::move(prom), paramsCopy, frontFaceRotation, viewportCopy]() mutable {
        try{
            GEViewport _viewport = {};
            GEViewport *viewportPtr = nullptr;
            if(viewportCopy.has_value()){
                _viewport = viewportCopy.value();
                viewportPtr = &_viewport;
            }
            promise.set_value_at_thread_exit(triangulateSync(paramsCopy,frontFaceRotation,viewportPtr));
        }
        catch(...){
            promise.set_exception(std::current_exception());
        }
    });
    {
        std::lock_guard<std::mutex> lock(activeThreadsMutex);
        activeThreads.emplace_back(std::move(worker));
    }
    return fut;
};

TETriangulationResult OmegaTriangulationEngineContext::triangulateSync(const TETriangulationParams &params,GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport){
    TETriangulationResult res;
    _triangulatePriv(params,frontFaceRotation,viewport,res);
    return res;
};

OmegaTriangulationEngineContext::~OmegaTriangulationEngineContext(){
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(activeThreadsMutex);
        threads.swap(activeThreads);
    }
    for(auto &t : threads){
        if(t.joinable()){
            t.join();
        }
    }
};

// The deprecated result-level wrappers forward to the deprecated TEMesh
// transforms; suppress the self-referential deprecation warning here (the
// attribute still fires at external call sites). Superseded by GESpace.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
void TETriangulationResult::translate(float x,float y,float z,const GEViewport & viewport){
    mesh.translate(x,y,z,viewport);
};

void TETriangulationResult::rotate(float pitch,float yaw,float roll){
    mesh.rotate(pitch,yaw,roll);
};

void TETriangulationResult::scale(float w,float h,float l){
    mesh.scale(w,h,l);
};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

void TETriangulationResult::TEMesh::translate(float x, float y, float z,const GEViewport & viewport) {
    auto m = translationMatrix(
        (2.f * x) / viewport.width,
        (2.f * y) / viewport.height,
        (2.f * z) / viewport.farDepth
    );
    for(auto & polygon : vertexPolygons){
        polygon.a.pt = transformPoint(m, polygon.a.pt);
        polygon.b.pt = transformPoint(m, polygon.b.pt);
        polygon.c.pt = transformPoint(m, polygon.c.pt);
    }
}

void TETriangulationResult::TEMesh::rotate(float pitch, float yaw, float roll) {
    auto m = rotationEuler(pitch, yaw, roll);
    for(auto & polygon : vertexPolygons){
        polygon.a.pt = transformPoint(m, polygon.a.pt);
        polygon.b.pt = transformPoint(m, polygon.b.pt);
        polygon.c.pt = transformPoint(m, polygon.c.pt);
    }
}

void TETriangulationResult::TEMesh::scale(float w, float h, float l) {
    auto m = scalingMatrix(w, h, l);
    for(auto & polygon : vertexPolygons){
        polygon.a.pt = transformPoint(m, polygon.a.pt);
        polygon.b.pt = transformPoint(m, polygon.b.pt);
        polygon.c.pt = transformPoint(m, polygon.c.pt);
    }
}



_NAMESPACE_END_
