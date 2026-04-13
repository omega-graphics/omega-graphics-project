#include "omegaGTE/TE.h"
#include "omegaGTE/GTEMath.h"
// #include "omegaGTE/GTEShaderTypes.h"
#include <array>
#include <optional>
#include <thread>
#include <iostream>
#include <cmath>
#include <exception>

_NAMESPACE_BEGIN_


struct TETriangulationParams::GraphicsPath3DParams {
    GVectorPath3D * pathes;
    unsigned pathCount;
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

    GraphicsPath3DParams path3D;

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

TETriangulationParams TETriangulationParams::GraphicsPath2D(GVectorPath2D & path,float strokeWidth,bool contour,bool fill){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.graphicsPath2D = std::make_shared<GVectorPath2D>(path);
    params.graphicsPath2DContour = contour;
    params.graphicsPath2DFill = fill;
    params.graphicsPath2DStrokeWidth = strokeWidth;
    params.type = params.params->type = TRIANGULATE_GRAPHICSPATH2D;
    return params;
};

TETriangulationParams TETriangulationParams::GraphicsPath3D(unsigned int vectorPathCount, GVectorPath3D *const vectorPaths){
    TETriangulationParams params;
    params.params.reset(new Data{});
    params.params->path3D = {vectorPaths,vectorPathCount};
    params.type = params.params->type =  TRIANGULATE_GRAPHICSPATH3D;
    return params;
};

unsigned int TETriangulationResult::TEMesh::vertexCount() {
    auto polygonCount = vertexPolygons.size();
    return polygonCount * 3;
}

unsigned int TETriangulationResult::totalVertexCount() {
    unsigned vertexCount = 0;
    for(auto & m : meshes){
        vertexCount += m.vertexCount();
    }
    return vertexCount;
}

TETriangulationParams::~TETriangulationParams() = default;


SharedHandle<OmegaTriangulationEngine> OmegaTriangulationEngine::Create(){
    return std::make_shared<OmegaTriangulationEngine>();
};

GEViewport OmegaTriangulationEngineContext::getEffectiveViewport(){
    return GEViewport{0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
}

void OmegaTriangulationEngineContext::translateCoordsDefaultImpl(float x, float y, float z, GEViewport * viewport, float *x_result, float *y_result, float *z_result){
    *x_result = (2.f * x / viewport->width) - 1.f;
    *y_result = (2.f * y / viewport->height) - 1.f;
    if(z_result != nullptr){
        if(z > 0.0){
            *z_result = (2.f * z / viewport->farDepth) - 1.f;
        }
        else if(z < 0.0){
            *z_result = (2.f * z / viewport->nearDepth) - 1.f;
        }
        else {
            *z_result = z;
        };
    };
};

inline void OmegaTriangulationEngineContext::_triangulatePriv(const TETriangulationParams &params,GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport,TETriangulationResult & result){
    assert(params.attachments.size() <= 2 && "At most 2 attachments are allowed for each tessellation params");
    GEViewport fallbackViewport = getEffectiveViewport();
    if (!viewport) viewport = &fallbackViewport;

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

    switch(params.type){
        case TETriangulationParams::TRIANGULATE_RECT : {
            std::cout << "Tessalate GRect" << std::endl;
            std::cout << "Viewport: x:" << viewport->x << " y:" << viewport->y << " w:" << viewport->width << " h:" << viewport->height << " " << std::endl;
            GRect &object = params.params->rect;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            TETriangulationResult::TEMesh::Polygon tri {};
            float x0,x1,y0,y1;
            float u,v;
            translateCoords(object.pos.x,object.pos.y,0.f,viewport,&x0,&y0,nullptr);
            translateCoords(object.pos.x + object.w,object.pos.y + object.h,0.f,viewport,&x1,&y1,nullptr);

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
                    
                    translateCoords(attachment.texture2DData.width,attachment.texture2DData.height,0, viewport,&u,&v,nullptr);
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

            mesh.vertexPolygons.push_back(tri);

            
            result.meshes.push_back(mesh);

            break;
        }
        case TETriangulationParams::TRIANGULATE_ROUNDEDRECT : {
            auto & object = params.params->rounded_rect;
            const float ox = object.pos.x;
            const float oy = object.pos.y;
            const float rad_x = std::fmax(0.0f,std::fmin(object.rad_x,object.w * 0.5f));
            const float rad_y = std::fmax(0.0f,std::fmin(object.rad_y,object.h * 0.5f));
            const size_t roundedRectStartIdx = result.meshes.size();
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(colorAttachmentPtr != nullptr){
                colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                        TETriangulationResult::AttachmentData{colorAttachmentPtr->colorData.color,FVec<2>::Create(),FVec<3>::Create()});
            }

            GRect middle_rect {GPoint2D{ox + rad_x, oy + rad_y},object.w - (2 * rad_x),object.h - (2 * rad_y)};

            auto middle_rect_params = TETriangulationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETriangulationParams::Attachment::makeColor(colorAttachment->color));
            }

            _triangulatePriv(middle_rect_params,frontFaceRotation,viewport,result);

            auto tessellateArc = [&](GPoint2D start, float rad_x, float rad_y, float angle_start, float angle_end, float _arcStep){
                if(std::fabs(_arcStep) < 0.0001f){
                    return;
                }
                TETriangulationResult::TEMesh m {TETriangulationResult::TEMesh::TopologyTriangleStrip};
                float centerX,centerY;
                translateCoords(start.x,start.y,0.f,viewport,&centerX,&centerY,nullptr);
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

                    auto x_f = cosf(angle) * rad_x;
                    auto y_f = sinf(angle) * rad_y;

                    x_f += start.x;
                    y_f += start.y;

                    p.a.pt = pt_a;
                    float x_t,y_t;
                    translateCoords(x_f,y_f,0.f,viewport,&x_t,&y_t,nullptr);
                    p.b.pt = GPoint3D {x_t,y_t,0.f};


                    x_f = cosf(nextAngle) * rad_x;
                    y_f = sinf(nextAngle) * rad_y;

                    x_f += start.x;
                    y_f += start.y;

                    translateCoords(x_f,y_f,0.f,viewport,&x_t,&y_t,nullptr);
                    p.c.pt = GPoint3D {x_t,y_t,0.f};
                    if(colorAttachment){
                        p.a.attachment = p.b.attachment = p.c.attachment = colorAttachment;
                    }

                    m.vertexPolygons.push_back(p);
                    angle = nextAngle;
                }
                result.meshes.push_back(m);
            };

            /// Bottom Left Arc
            tessellateArc(GPoint2D {ox + rad_x, oy + rad_y}, rad_x, rad_y, float(3.f * PI) / 2.f, PI, -arcStep);

            /// Left Rect
            middle_rect = GRect {GPoint2D{ox, oy + rad_y},rad_x,object.h - (2 * rad_y)};
            middle_rect_params = TETriangulationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETriangulationParams::Attachment::makeColor(colorAttachment->color));
            }

            _triangulatePriv(middle_rect_params,frontFaceRotation,viewport,result);
            /// Top Left Arc
            tessellateArc(GPoint2D {ox + rad_x, oy + object.h - rad_y}, rad_x, rad_y, PI, float(PI) / 2.f, -arcStep);

            /// Top Rect
            middle_rect = GRect {GPoint2D{ox + rad_x, oy + object.h - rad_y},object.w - (rad_x * 2),rad_y};
            middle_rect_params = TETriangulationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETriangulationParams::Attachment::makeColor(colorAttachment->color));
            }

            _triangulatePriv(middle_rect_params,frontFaceRotation,viewport,result);
            /// Top Right Arc
            tessellateArc(GPoint2D {ox + object.w - rad_x, oy + object.h - rad_y}, rad_x, rad_y, float(PI) / 2.f, 0, -arcStep);

            /// Right Rect
            middle_rect = GRect {GPoint2D{ox + object.w - rad_x, oy + rad_y},rad_x,object.h - (2 * rad_y)};
            middle_rect_params = TETriangulationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETriangulationParams::Attachment::makeColor(colorAttachment->color));
            }

            _triangulatePriv(middle_rect_params,frontFaceRotation,viewport,result);

            /// Bottom Right Arc
            tessellateArc(GPoint2D {ox + object.w - rad_x, oy + rad_y}, rad_x, rad_y, 0, -float(PI) / 2.f, -arcStep);

            /// Bottom Rect
            middle_rect = GRect {GPoint2D{ox + rad_x, oy},object.w - (rad_x * 2),rad_y};
            middle_rect_params = TETriangulationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETriangulationParams::Attachment::makeColor(colorAttachment->color));
            }
            _triangulatePriv(middle_rect_params,frontFaceRotation,viewport,result);

            if(textureAttachment != nullptr && textureAttachment->type == TETriangulationParams::Attachment::TypeTexture2D){
                float dx0,dy0,dx1,dy1;
                translateCoords(ox,oy,0.f,viewport,&dx0,&dy0,nullptr);
                translateCoords(ox + object.w,oy + object.h,0.f,viewport,&dx1,&dy1,nullptr);
                const float rangeX = (dx1 - dx0);
                const float rangeY = (dy1 - dy0);
                FVec<3> normal = makeVec3(0.f,0.f,1.f);
                for(size_t i = roundedRectStartIdx; i < result.meshes.size(); ++i){
                    auto & m = result.meshes[i];
                    for(auto & poly : m.vertexPolygons){
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
            translateCoords(object.x,object.y,object.z,viewport,&centerX,&centerY,&centerZ);

            auto makePoint = [&](float angle){
                float x = object.x + std::cos(angle) * object.rad_x;
                float y = object.y + std::sin(angle) * object.rad_y;
                float tx,ty,tz;
                translateCoords(x,y,object.z,viewport,&tx,&ty,&tz);
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

            result.meshes.push_back(mesh);
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
            translateCoords(object.pos.x,object.pos.y,object.pos.z,viewport,&x0,&y0,&z0);
            translateCoords(object.pos.x + object.w,
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

            result.meshes.push_back(mesh);

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
                translateCoords(point.x,point.y,0.f,viewport,&x,&y,nullptr);
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
                    TETriangulationResult::TEMesh fillMesh {TETriangulationResult::TEMesh::TopologyTriangle};
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
                        fillMesh.vertexPolygons.push_back(tri);
                    }
                    if(!fillMesh.vertexPolygons.empty()){
                        result.meshes.push_back(fillMesh);
                    }
                }
            }

            // --- Stroke ---
            const float strokeWidth = params.graphicsPath2DStrokeWidth > 0.f ? params.graphicsPath2DStrokeWidth : 0.f;
            if(strokeWidth > 0.f){
                TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
                float strokeCursor = 0.f;

                auto appendStrokeSegment = [&](const GPoint2D & start,const GPoint2D & end){
                    const float dx = end.x - start.x;
                    const float dy = end.y - start.y;
                    const float len = std::sqrt((dx * dx) + (dy * dy));
                    if(len <= 0.000001f){
                        return;
                    }

                    const float halfStroke = strokeWidth * 0.5f;
                    const float nx = -dy / len;
                    const float ny = dx / len;

                    const GPoint2D a{start.x + nx * halfStroke,start.y + ny * halfStroke};
                    const GPoint2D b{start.x - nx * halfStroke,start.y - ny * halfStroke};
                    const GPoint2D c{end.x + nx * halfStroke,end.y + ny * halfStroke};
                    const GPoint2D d{end.x - nx * halfStroke,end.y - ny * halfStroke};

                    const float uStart = hasTex2D ? (strokeCursor / totalLen) : 0.f;
                    const float uEnd = hasTex2D ? ((strokeCursor + len) / totalLen) : 0.f;
                    strokeCursor += len;

                    auto strokeAttachment = [&](float u, float v) -> std::optional<TETriangulationResult::AttachmentData> {
                        if(hasTex2D){
                            return std::make_optional<TETriangulationResult::AttachmentData>(
                                makeTex2DAttachment(u, v, pathNormal));
                        }
                        return colorAttachment;
                    };

                    TETriangulationResult::TEMesh::Polygon p {};
                    p.a.pt = toDevicePoint(a);
                    p.b.pt = toDevicePoint(b);
                    p.c.pt = toDevicePoint(c);
                    p.a.attachment = strokeAttachment(uStart, 0.f);
                    p.b.attachment = strokeAttachment(uStart, 1.f);
                    p.c.attachment = strokeAttachment(uEnd, 0.f);
                    mesh.vertexPolygons.push_back(p);

                    p.a.pt = toDevicePoint(c);
                    p.b.pt = toDevicePoint(b);
                    p.c.pt = toDevicePoint(d);
                    p.a.attachment = strokeAttachment(uEnd, 0.f);
                    p.b.attachment = strokeAttachment(uStart, 1.f);
                    p.c.attachment = strokeAttachment(uEnd, 1.f);
                    mesh.vertexPolygons.push_back(p);
                };

                for(auto path_it = path.begin();path_it != path.end();path_it.operator++()){
                    auto segment = *path_it;
                    if(segment.pt_A == nullptr || segment.pt_B == nullptr){
                        break;
                    }
                    appendStrokeSegment(*segment.pt_A,*segment.pt_B);
                }

                if(params.graphicsPath2DContour && path.size() >= 2){
                    appendStrokeSegment(path.lastPt(),path.firstPt());
                }

                if(!mesh.vertexPolygons.empty()){
                    result.meshes.push_back(mesh);
                }
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
            translateCoords(object.x, object.y + object.h, object.z, viewport, &ax, &ay, &az);
            GPoint3D apex {ax, ay, az};

            float bx0,by0,bz0, bx1,by1,bz1;
            translateCoords(object.x - object.w * 0.5f, object.y, object.z - object.d * 0.5f, viewport, &bx0, &by0, &bz0);
            translateCoords(object.x + object.w * 0.5f, object.y, object.z + object.d * 0.5f, viewport, &bx1, &by1, &bz1);

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

            result.meshes.push_back(mesh);
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
            translateCoords(object.pos.x, object.pos.y, object.pos.z, viewport, &cx_bottom, &cy_bottom, &cz_bottom);
            translateCoords(object.pos.x, object.pos.y + object.h, object.pos.z, viewport, &cx_top, &cy_top, &cz_top);

            GPoint3D bottomCenter {cx_bottom, cy_bottom, cz_bottom};
            GPoint3D topCenter {cx_top, cy_top, cz_top};

            const float step = arcStep > 0.f ? arcStep : 0.01f;
            float angle = 0.f;

            auto makeRimPoint = [&](float a, float baseY) -> GPoint3D {
                float px = object.pos.x + std::cos(a) * object.r;
                float pz = object.pos.z + std::sin(a) * object.r;
                float tx, ty, tz;
                translateCoords(px, baseY, pz, viewport, &tx, &ty, &tz);
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

            result.meshes.push_back(mesh);
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
            translateCoords(object.x, object.y + object.h, object.z, viewport, &apex_tx, &apex_ty, &apex_tz);
            GPoint3D apex {apex_tx, apex_ty, apex_tz};

            float base_cx,base_cy,base_cz;
            translateCoords(object.x, object.y, object.z, viewport, &base_cx, &base_cy, &base_cz);
            GPoint3D baseCenter {base_cx, base_cy, base_cz};

            const float step = arcStep > 0.f ? arcStep : 0.01f;
            float angle = 0.f;

            auto makeBasePoint = [&](float a) -> GPoint3D {
                float px = object.x + std::cos(a) * object.r;
                float pz = object.z + std::sin(a) * object.r;
                float tx,ty,tz;
                translateCoords(px, object.y, pz, viewport, &tx, &ty, &tz);
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

            result.meshes.push_back(mesh);
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

            const float strokeWidth = 1.f;
            const float halfStroke = strokeWidth * 0.5f;

            auto toDevice3D = [&](const GPoint3D & point) -> GPoint3D {
                float tx,ty,tz;
                translateCoords(point.x, point.y, point.z, viewport, &tx, &ty, &tz);
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
                result.meshes.push_back(mesh);
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
SharedHandle<OmegaTriangulationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> & renderTarget);

SharedHandle<OmegaTriangulationEngineContext> OmegaTriangulationEngine::createTEContextFromNativeRenderTarget(SharedHandle<GENativeRenderTarget> & renderTarget){
    return CreateNativeRenderTargetTEContext(renderTarget);
};

SharedHandle<OmegaTriangulationEngineContext> OmegaTriangulationEngine::createTEContextFromTextureRenderTarget(SharedHandle<GETextureRenderTarget> & renderTarget){
    return CreateTextureRenderTargetTEContext(renderTarget);
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

void TETriangulationResult::translate(float x,float y,float z,const GEViewport & viewport){
    for(auto & m : meshes){
        m.translate(x,y,z,viewport);
    }
};

void TETriangulationResult::rotate(float pitch,float yaw,float roll){
    for(auto & m : meshes){
        m.rotate(pitch,yaw,roll);
    }
};

void TETriangulationResult::scale(float w,float h,float l){
    for(auto & m : meshes){
        m.scale(w,h,l);
    }
};

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
