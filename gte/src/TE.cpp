#include "omegaGTE/TE.h"
// #include "omegaGTE/GTEShaderTypes.h"
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

TETriangulationParams::Attachment TETriangulationParams::Attachment::makeTexture2D(unsigned width,unsigned height) {
   Attachment at{TypeTexture2D};
   at.colorData.color.~Matrix();
   at.texture2DData.width = width;
   at.texture2DData.height = height;
   return at;
}

TETriangulationParams::Attachment TETriangulationParams::Attachment::makeTexture3D(unsigned width,unsigned height,unsigned depth) {
    Attachment at{TypeTexture3D};
   at.colorData.color.~Matrix();
   at.texture3DData.width = width;
   at.texture3DData.height = height;
   at.texture3DData.depth = depth;
   return at;
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
    assert(params.attachments.size() <= 1 && "Only 1 attachment is allowed for each tessellation params");
    GEViewport fallbackViewport = getEffectiveViewport();
    if (!viewport) viewport = &fallbackViewport;

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
            const float rad_x = std::fmax(0.0f,std::fmin(object.rad_x,object.w * 0.5f));
            const float rad_y = std::fmax(0.0f,std::fmin(object.rad_y,object.h * 0.5f));
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETriangulationParams::Attachment::TypeColor){
                    colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                            TETriangulationResult::AttachmentData{attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
                }
            }

            GRect middle_rect {rad_x,rad_y,object.w - (2 * rad_x),object.h - (2 * rad_y)};

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
            tessellateArc(GPoint2D {rad_x, rad_y}, rad_x, rad_y, float(3.f * PI) / 2.f, PI, -arcStep);

            /// Left Rect
            middle_rect = GRect {GPoint2D{0.f,rad_y},rad_x,object.h - (2 * rad_y)};
            middle_rect_params = TETriangulationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETriangulationParams::Attachment::makeColor(colorAttachment->color));
            }

            _triangulatePriv(middle_rect_params,frontFaceRotation,viewport,result);
            /// Top Left Arc
            tessellateArc(GPoint2D {rad_x, object.h - rad_y}, rad_x, rad_y, PI, float(PI) / 2.f, -arcStep);

            /// Top Rect
            middle_rect = GRect {GPoint2D{rad_x,object.h - rad_y},object.w - (rad_x * 2),rad_y};
            middle_rect_params = TETriangulationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETriangulationParams::Attachment::makeColor(colorAttachment->color));
            }

            _triangulatePriv(middle_rect_params,frontFaceRotation,viewport,result);
            /// Top Right Arc
            tessellateArc(GPoint2D {object.w - rad_x, object.h - rad_y}, rad_x, rad_y, float(PI) / 2.f, 0, -arcStep);

            /// Right Rect
            middle_rect = GRect {GPoint2D{object.w - rad_x,rad_y},rad_x,object.h - (2 * rad_y)};
            middle_rect_params = TETriangulationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETriangulationParams::Attachment::makeColor(colorAttachment->color));
            }

            _triangulatePriv(middle_rect_params,frontFaceRotation,viewport,result);

            /// Bottom Right Arc
            tessellateArc(GPoint2D {object.w - rad_x, rad_y}, rad_x, rad_y, 0, -float(PI) / 2.f, -arcStep);

            /// Bottom Rect
            middle_rect = GRect {GPoint2D{rad_x,0.f},object.w - (rad_x * 2),rad_y};
            middle_rect_params = TETriangulationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETriangulationParams::Attachment::makeColor(colorAttachment->color));
            }
            _triangulatePriv(middle_rect_params,frontFaceRotation,viewport,result);

            break;
        }
        case TETriangulationParams::TRIANGULATE_ELLIPSOID : {
            auto & object = params.params->ellipsoid;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETriangulationParams::Attachment::TypeColor){
                    colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                            TETriangulationResult::AttachmentData{attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
                }
            }

            float centerX,centerY,centerZ;
            translateCoords(object.x,object.y,object.z,viewport,&centerX,&centerY,&centerZ);

            auto makePoint = [&](float angle){
                float x = object.x + std::cos(angle) * object.rad_x;
                float y = object.y + std::sin(angle) * object.rad_y;
                float tx,ty,tz;
                translateCoords(x,y,object.z,viewport,&tx,&ty,&tz);
                return GPoint3D{tx,ty,tz};
            };

            auto center = GPoint3D{centerX,centerY,centerZ};
            const float step = arcStep > 0.f ? arcStep : 0.01f;
            float angle = 0.f;
            auto prev = makePoint(0.f);

            while(angle < (2.f * PI)){
                float nextAngle = angle + step;
                if(nextAngle > (2.f * PI)){
                    nextAngle = 2.f * PI;
                }
                auto next = makePoint(nextAngle);

                TETriangulationResult::TEMesh::Polygon tri {};
                tri.a.pt = center;
                tri.b.pt = prev;
                tri.c.pt = next;
                if(colorAttachment){
                    tri.a.attachment = tri.b.attachment = tri.c.attachment = colorAttachment;
                }
                mesh.vertexPolygons.push_back(tri);

                prev = next;
                angle = nextAngle;
            }

            result.meshes.push_back(mesh);
            break;
        }
        case TETriangulationParams::TRIANGULATE_RECTANGULAR_PRISM : {
            auto & object = params.params->prism;


            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            TETriangulationResult::TEMesh::Polygon tri {};

            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETriangulationParams::Attachment::TypeColor) {
                    tri.a.attachment = tri.b.attachment = tri.c.attachment = std::make_optional<TETriangulationResult::AttachmentData>(
                            {attachment.colorData.color, FVec<2>::Create(), FVec<3>::Create()});
                }
            }

            float x0,x1,y0,y1,z0,z1;
            translateCoords(object.pos.x,object.pos.y,object.pos.z,viewport,&x0,&y0,&z0);
            translateCoords(object.pos.x + object.w,
                            object.pos.y + object.h,
                            object.pos.z + object.d,
                             viewport,&x1,&y1,&z1);

            /// Bottom Side

            tri.a.pt = GPoint3D {x0,y0,z0};
            tri.b.pt = GPoint3D {x1,y0,z0};
            tri.c.pt = GPoint3D {x1,y0,z1};

            mesh.vertexPolygons.push_back(tri);

            tri.b.pt = GPoint3D {x0,y0,z1};

            mesh.vertexPolygons.push_back(tri);

            /// Front Side

            tri.b.pt = GPoint3D {x0,y1,z0};
            tri.c.pt = GPoint3D {x1,y1,z0};

            mesh.vertexPolygons.push_back(tri);

            tri.b.pt = GPoint3D {x1,y0,z0};

            mesh.vertexPolygons.push_back(tri);

            /// Left Side

            tri.a.pt = GPoint3D {x1,y0,z0};
            tri.b.pt = GPoint3D {x1,y1,z0};
            tri.c.pt = GPoint3D {x1,y1,z1};

            mesh.vertexPolygons.push_back(tri);

            tri.b.pt = GPoint3D {x1,y0,z1};

            mesh.vertexPolygons.push_back(tri);

            /// Right Side

            tri.a.pt = GPoint3D {x0,y0,z0};
            tri.b.pt = GPoint3D {x0,y1,z0};
            tri.c.pt = GPoint3D {x0,y1,z1};

            mesh.vertexPolygons.push_back(tri);

            tri.b.pt = GPoint3D {x0,y0,z1};

            mesh.vertexPolygons.push_back(tri);

            /// Back Side

            tri.a.pt = GPoint3D {x0,y0,z1};
            tri.b.pt = GPoint3D {x0,y1,z1};
            tri.c.pt = GPoint3D {x1,y1,z1};

            mesh.vertexPolygons.push_back(tri);

            tri.b.pt = GPoint3D {x1,y0,z1};

            mesh.vertexPolygons.push_back(tri);

            /// Top Side

            tri.a.pt = GPoint3D {x0,y1,z0};
            tri.b.pt = GPoint3D {x0,y1,z1};
            tri.c.pt = GPoint3D {x1,y1,z1};

            mesh.vertexPolygons.push_back(tri);

            tri.b.pt = GPoint3D {x1,y1,z0};

            mesh.vertexPolygons.push_back(tri);

            /// Finish
            result.meshes.push_back(mesh);

            break;
        }
        case TETriangulationParams::TRIANGULATE_GRAPHICSPATH2D : {
            if(params.graphicsPath2D == nullptr){
                break;
            }
            auto & path = *params.graphicsPath2D;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETriangulationParams::Attachment::TypeColor){
                    colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                            TETriangulationResult::AttachmentData{attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
                }
            }

            const float strokeWidth = params.graphicsPath2DStrokeWidth > 0.f ? params.graphicsPath2DStrokeWidth : 1.f;
            auto toDevicePoint = [&](const GPoint2D &point){
                float x,y;
                // Path points are provided in canvas pixel-space (top-left origin),
                // so shift by half viewport like other 2D primitives before NDC conversion.
                const float px = point.x - (viewport->width * 0.5f);
                const float py = (viewport->height * 0.5f) - point.y;
                translateCoords(px,py,0.f,viewport,&x,&y,nullptr);
                return GPoint3D{x,y,0.f};
            };

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

                TETriangulationResult::TEMesh::Polygon p {};
                p.a.pt = toDevicePoint(a);
                p.b.pt = toDevicePoint(b);
                p.c.pt = toDevicePoint(c);
                if(colorAttachment){
                    p.a.attachment = p.b.attachment = p.c.attachment = colorAttachment;
                }
                mesh.vertexPolygons.push_back(p);

                p.a.pt = toDevicePoint(c);
                p.b.pt = toDevicePoint(b);
                p.c.pt = toDevicePoint(d);
                if(colorAttachment){
                    p.a.attachment = p.b.attachment = p.c.attachment = colorAttachment;
                }
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
            break;
        }
        case TETriangulationParams::TRIANGULATE_PYRAMID : {
            auto & object = params.params->pyramid;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            TETriangulationResult::TEMesh::Polygon tri {};

            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETriangulationParams::Attachment::TypeColor){
                    colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                            TETriangulationResult::AttachmentData{attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
                    tri.a.attachment = tri.b.attachment = tri.c.attachment = colorAttachment;
                }
            }

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

            // Front face
            tri.a.pt = apex; tri.b.pt = b00; tri.c.pt = b10;
            mesh.vertexPolygons.push_back(tri);
            // Right face
            tri.a.pt = apex; tri.b.pt = b10; tri.c.pt = b11;
            mesh.vertexPolygons.push_back(tri);
            // Back face
            tri.a.pt = apex; tri.b.pt = b11; tri.c.pt = b01;
            mesh.vertexPolygons.push_back(tri);
            // Left face
            tri.a.pt = apex; tri.b.pt = b01; tri.c.pt = b00;
            mesh.vertexPolygons.push_back(tri);
            // Base (two triangles)
            tri.a.pt = b00; tri.b.pt = b10; tri.c.pt = b11;
            mesh.vertexPolygons.push_back(tri);
            tri.a.pt = b00; tri.b.pt = b11; tri.c.pt = b01;
            mesh.vertexPolygons.push_back(tri);

            result.meshes.push_back(mesh);
            break;
        }
        case TETriangulationParams::TRIANGULATE_CYLINDER : {
            auto & object = params.params->cylinder;

            TETriangulationResult::TEMesh mesh {TETriangulationResult::TEMesh::TopologyTriangle};
            std::optional<TETriangulationResult::AttachmentData> colorAttachment;
            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETriangulationParams::Attachment::TypeColor){
                    colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                            TETriangulationResult::AttachmentData{attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
                }
            }

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

            while(angle < 2.f * float(PI)){
                float nextAngle = angle + step;
                if(nextAngle > 2.f * float(PI)) nextAngle = 2.f * float(PI);

                GPoint3D bCur = makeRimPoint(angle, object.pos.y);
                GPoint3D bNext = makeRimPoint(nextAngle, object.pos.y);
                GPoint3D tCur = makeRimPoint(angle, object.pos.y + object.h);
                GPoint3D tNext = makeRimPoint(nextAngle, object.pos.y + object.h);

                TETriangulationResult::TEMesh::Polygon p {};
                if(colorAttachment){
                    p.a.attachment = p.b.attachment = p.c.attachment = colorAttachment;
                }

                // Bottom cap triangle
                p.a.pt = bottomCenter; p.b.pt = bCur; p.c.pt = bNext;
                mesh.vertexPolygons.push_back(p);
                // Top cap triangle
                p.a.pt = topCenter; p.b.pt = tNext; p.c.pt = tCur;
                mesh.vertexPolygons.push_back(p);
                // Barrel quad (two triangles)
                p.a.pt = bCur; p.b.pt = tCur; p.c.pt = tNext;
                mesh.vertexPolygons.push_back(p);
                p.a.pt = bCur; p.b.pt = tNext; p.c.pt = bNext;
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
            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETriangulationParams::Attachment::TypeColor){
                    colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                            TETriangulationResult::AttachmentData{attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
                }
            }

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

            while(angle < 2.f * float(PI)){
                float nextAngle = angle + step;
                if(nextAngle > 2.f * float(PI)) nextAngle = 2.f * float(PI);

                GPoint3D cur = makeBasePoint(angle);
                GPoint3D next = makeBasePoint(nextAngle);

                TETriangulationResult::TEMesh::Polygon p {};
                if(colorAttachment){
                    p.a.attachment = p.b.attachment = p.c.attachment = colorAttachment;
                }

                // Side triangle
                p.a.pt = apex; p.b.pt = cur; p.c.pt = next;
                mesh.vertexPolygons.push_back(p);
                // Base triangle
                p.a.pt = baseCenter; p.b.pt = next; p.c.pt = cur;
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
            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETriangulationParams::Attachment::TypeColor){
                    colorAttachment = std::make_optional<TETriangulationResult::AttachmentData>(
                            TETriangulationResult::AttachmentData{attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
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

                    TETriangulationResult::TEMesh::Polygon p {};
                    if(colorAttachment){
                        p.a.attachment = p.b.attachment = p.c.attachment = colorAttachment;
                    }
                    p.a.pt = a0; p.b.pt = a1; p.c.pt = b0;
                    mesh.vertexPolygons.push_back(p);
                    p.a.pt = b0; p.b.pt = a1; p.c.pt = b1;
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
    auto _x = (2 * x)/viewport.width;
    auto _y = (2 * y)/viewport.height;
    auto _z = (2 * z)/viewport.farDepth;

    for(auto & polygon : vertexPolygons){
        polygon.a.pt.x += _x;
        polygon.b.pt.x += _x;
        polygon.c.pt.x += _x;

        polygon.a.pt.y += _y;
        polygon.b.pt.y += _y;
        polygon.c.pt.y += _y;

        polygon.a.pt.z += _z;
        polygon.b.pt.z += _z;
        polygon.c.pt.z += _z;
    }
}

void TETriangulationResult::TEMesh::rotate(float pitch, float yaw, float roll) {

    /// Pitch Rotation -- X Axis.
    /// Yaw Rotation -- Y Axis.
    /// Roll Rotation -- Z Axis.

    auto cos_pitch = cosf(pitch),sin_pitch = sinf(pitch);
    auto cos_yaw = cosf(yaw),sin_yaw =sinf(yaw);
    auto cos_roll = cosf(roll),sin_roll = sinf(roll);

    auto rotatePoint = [&](GPoint3D & pt){
        float x = pt.x, y = pt.y, z = pt.z;
        /// Pitch Rotation (X axis)
        float y1 = (cos_pitch * y) - (sin_pitch * z);
        float z1 = (sin_pitch * y) + (cos_pitch * z);
        /// Yaw Rotation (Y axis)
        float x2 = (cos_yaw * x) + (sin_yaw * z1);
        float z2 = -(sin_yaw * x) + (cos_yaw * z1);
        /// Roll Rotation (Z axis)
        pt.x = (cos_roll * x2) - (sin_roll * y1);
        pt.y = (sin_roll * x2) + (cos_roll * y1);
        pt.z = z2;
    };

    for(auto & polygon : vertexPolygons){
        rotatePoint(polygon.a.pt);
        rotatePoint(polygon.b.pt);
        rotatePoint(polygon.c.pt);
    }
}

void TETriangulationResult::TEMesh::scale(float w, float h,float l) {
    for(auto & polygon : vertexPolygons){
        polygon.a.pt.x *= w;
        polygon.b.pt.x *= w;
        polygon.c.pt.x *= w;

        polygon.a.pt.y *= h;
        polygon.b.pt.y *= h;
        polygon.c.pt.y *= h;

        polygon.a.pt.z *= l;
        polygon.b.pt.z *= l;
        polygon.c.pt.z *= l;
    }
}



_NAMESPACE_END_
