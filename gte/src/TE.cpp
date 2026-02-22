#include "omegaGTE/TE.h"
// #include "omegaGTE/GTEShaderTypes.h"
#include <optional>
#include <thread>
#include <iostream>
#include <cmath>
#include <exception>

_NAMESPACE_BEGIN_


struct TETessellationParams::GraphicsPath3DParams {
    GVectorPath3D * pathes;
    unsigned pathCount;
};

union TETessellationParams::Data {

    TessalationType type;

    GRect rect;

    GRoundedRect rounded_rect;
    
    GCone cone;

    GEllipsoid ellipsoid;

    GRectangularPrism prism;

    GPyramid pyramid;

    GCylinder cylinder;

    GraphicsPath3DParams path3D;

    ~Data(){
        if(type == TESSALATE_RECT){

        }
    };
};


TETessellationParams::Attachment TETessellationParams::Attachment::makeColor(const FVec<4> &color) {
    Attachment at{TypeColor};
    at.colorData.color = color;
    return at;
}

TETessellationParams::Attachment TETessellationParams::Attachment::makeTexture2D(unsigned width,unsigned height) {
   Attachment at{TypeTexture2D};
   at.colorData.color.~Matrix();
   at.texture2DData.width = width;
   at.texture2DData.height = height;
   return at;
}

TETessellationParams::Attachment TETessellationParams::Attachment::makeTexture3D(unsigned width,unsigned height,unsigned depth) {
    Attachment at{TypeTexture3D};
   at.colorData.color.~Matrix();
   at.texture3DData.width = width;
   at.texture3DData.height = height;
   at.texture3DData.depth = depth;
   return at;
}

TETessellationParams::Attachment::~Attachment(){
    if(type == TypeColor){
        colorData.color.~Matrix();
    }
}

void TETessellationParams::addAttachment(const Attachment &attachment) {
    attachments.push_back(attachment);
}

TETessellationParams TETessellationParams::Rect(GRect &rect){
    TETessellationParams params;
    params.params.reset(new Data{});
    params.type = params.params->type = TESSALATE_RECT;
    params.params->rect = rect;
    return params;
};

TETessellationParams TETessellationParams::RoundedRect(GRoundedRect &roundedRect){
    TETessellationParams params;
    params.params.reset(new Data{});
    params.type = params.params->type = TESSALATE_ROUNDEDRECT;
    params.params->rounded_rect = roundedRect;
    return params;
};

TETessellationParams TETessellationParams::RectangularPrism(GRectangularPrism &prism){
    TETessellationParams params;
    params.params.reset(new Data{});
    // params.params.get_deleter().t = params.type;
    params.params->prism = prism;
    params.type = params.params->type = TESSELLATE_RECTANGULAR_PRISM;
    return params;
};

TETessellationParams TETessellationParams::Pyramid(GPyramid &pyramid){
    TETessellationParams params;
    params.params.reset(new Data{});
    params.params->pyramid = pyramid;
    params.type = params.params->type =  TESSALATE_PYRAMID;
    return params;
};

TETessellationParams TETessellationParams::Ellipsoid(GEllipsoid &ellipsoid){
    TETessellationParams params;
    params.params.reset(new Data{});
    params.params->ellipsoid = ellipsoid;
    params.type = params.params->type =  TESSALATE_ELLIPSOID;
    return params;
};

TETessellationParams TETessellationParams::Cylinder(GCylinder &cylinder){
    TETessellationParams params;
    params.params.reset(new Data{});
    params.params->cylinder = cylinder;
    params.type = params.params->type =  TESSALATE_CYLINDER;
    return params;
};

TETessellationParams TETessellationParams::Cone(GCone &cone){
    TETessellationParams params;
    params.params.reset(new Data{});
    params.params->cone = cone;
    params.type = params.params->type =  TESSALATE_CONE;
    return params;
};

TETessellationParams TETessellationParams::GraphicsPath2D(GVectorPath2D & path,float strokeWidth,bool contour,bool fill){
    TETessellationParams params;
    params.params.reset(new Data{});
    params.graphicsPath2D = std::make_shared<GVectorPath2D>(path);
    params.graphicsPath2DContour = contour;
    params.graphicsPath2DFill = fill;
    params.graphicsPath2DStrokeWidth = strokeWidth;
    params.type = params.params->type = TESSALATE_GRAPHICSPATH2D;
    return params;
};

TETessellationParams TETessellationParams::GraphicsPath3D(unsigned int vectorPathCount, GVectorPath3D *const vectorPaths){
    TETessellationParams params;
    params.params.reset(new Data{});
    params.params->path3D = {vectorPaths,vectorPathCount};
    params.type = params.params->type =  TESSALATE_GRAPHICSPATH3D;
    return params;
};

unsigned int TETessellationResult::TEMesh::vertexCount() {
    auto polygonCount = vertexPolygons.size();
    return polygonCount * 3;
}

unsigned int TETessellationResult::totalVertexCount() {
    unsigned vertexCount = 0;
    for(auto & m : meshes){
        vertexCount += m.vertexCount();
    }
    return vertexCount;
}

TETessellationParams::~TETessellationParams() = default;


SharedHandle<OmegaTessellationEngine> OmegaTessellationEngine::Create(){
    return std::make_shared<OmegaTessellationEngine>();
};

void OmegaTessellationEngineContext::translateCoordsDefaultImpl(float x, float y, float z, GEViewport * viewport, float *x_result, float *y_result, float *z_result){
    *x_result = (2 * x) / viewport->width;
    *y_result = (2 * y) / viewport->height;
    if(z_result != nullptr){
        if(z > 0.0){
            *z_result = (2 *z) / viewport->farDepth;
        }
        else if(z < 0.0){
            *z_result = (2 *z) / viewport->nearDepth;
        }
        else {
            *z_result = z;
        };
    };
};

inline void OmegaTessellationEngineContext::_tessalatePriv(const TETessellationParams &params,GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport,TETessellationResult & result){
    assert(params.attachments.size() <= 1 && "Only 1 attachment is allowed for each tessellation params");

    switch(params.type){
        case TETessellationParams::TESSALATE_RECT : {
            std::cout << "Tessalate GRect" << std::endl;
            std::cout << "Viewport: x:" << viewport->x << " y:" << viewport->y << " w:" << viewport->width << " h:" << viewport->height << " " << std::endl;
            GRect &object = params.params->rect;

            TETessellationResult::TEMesh mesh {TETessellationResult::TEMesh::TopologyTriangle};
            TETessellationResult::TEMesh::Polygon tri {};
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

            std::optional<TETessellationResult::AttachmentData> extra;

            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETessellationParams::Attachment::TypeColor){
                    extra = std::make_optional<TETessellationResult::AttachmentData>({attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
                    tri.a.attachment = tri.b.attachment = tri.c.attachment = extra;
                }
                else if(attachment.type == TETessellationParams::Attachment::TypeTexture2D){
                    
                    translateCoords(attachment.texture2DData.width,attachment.texture2DData.height,0, viewport,&u,&v,nullptr);
                    auto texCoord = FVec<2>::Create();

                    texCoord[0][0] = 0;
                    texCoord[1][0] = 0;

                    extra = std::make_optional<TETessellationResult::AttachmentData>({FVec<4>::Create(),texCoord,FVec<3>::Create()});
                    tri.b.attachment = extra;

                    texCoord[1][0] = 1;
                    extra = std::make_optional<TETessellationResult::AttachmentData>({FVec<4>::Create(),texCoord,FVec<3>::Create()});
                     tri.a.attachment = extra;

                     texCoord[0][0] = 1;
                      texCoord[1][0] = 1;
                    extra = std::make_optional<TETessellationResult::AttachmentData>({FVec<4>::Create(),texCoord,FVec<3>::Create()});
                     tri.c.attachment = extra;
                }
            }
            

            mesh.vertexPolygons.push_back(tri);

            tri.a.pt.x = x1;
            tri.a.pt.y = y1;

            if(!params.attachments.empty()){
                 auto & attachment = params.attachments.front();
                 if(attachment.type == TETessellationParams::Attachment::TypeTexture2D){
                     auto texCoord = FVec<2>::Create();
                     texCoord[0][0] = 1;
                     texCoord[1][0] = 0;
                     extra = std::make_optional<TETessellationResult::AttachmentData>({FVec<4>::Create(),texCoord,FVec<3>::Create()});
                    tri.a.attachment = extra;
                 }
            }

            mesh.vertexPolygons.push_back(tri);

            
            result.meshes.push_back(mesh);

            break;
        }
        case TETessellationParams::TESSALATE_ROUNDEDRECT : {
            auto & object = params.params->rounded_rect;
            const float rad_x = std::fmax(0.0f,std::fmin(object.rad_x,object.w * 0.5f));
            const float rad_y = std::fmax(0.0f,std::fmin(object.rad_y,object.h * 0.5f));
            std::optional<TETessellationResult::AttachmentData> colorAttachment;
            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETessellationParams::Attachment::TypeColor){
                    colorAttachment = std::make_optional<TETessellationResult::AttachmentData>(
                            TETessellationResult::AttachmentData{attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
                }
            }

            GRect middle_rect {rad_x,rad_y,object.w - (2 * rad_x),object.h - (2 * rad_y)};

            auto middle_rect_params = TETessellationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETessellationParams::Attachment::makeColor(colorAttachment->color));
            }

            _tessalatePriv(middle_rect_params,frontFaceRotation,viewport,result);

            auto tessellateArc = [&](GPoint2D start, float rad_x, float rad_y, float angle_start, float angle_end, float _arcStep){
                if(std::fabs(_arcStep) < 0.0001f){
                    return;
                }
                TETessellationResult::TEMesh m {TETessellationResult::TEMesh::TopologyTriangleStrip};
                float centerX,centerY;
                translateCoords(start.x,start.y,0.f,viewport,&centerX,&centerY,nullptr);
                GPoint3D pt_a {centerX,centerY,0.f};
                float angle = angle_start;
                while((_arcStep > 0.f) ? (angle < angle_end) : (angle > angle_end)){
                    TETessellationResult::TEMesh::Polygon p {};

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
            middle_rect_params = TETessellationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETessellationParams::Attachment::makeColor(colorAttachment->color));
            }

            _tessalatePriv(middle_rect_params,frontFaceRotation,viewport,result);
            /// Top Left Arc
            tessellateArc(GPoint2D {rad_x, object.h - rad_y}, rad_x, rad_y, PI, float(PI) / 2.f, -arcStep);

            /// Top Rect
            middle_rect = GRect {GPoint2D{rad_x,object.h - rad_y},object.w - (rad_x * 2),rad_y};
            middle_rect_params = TETessellationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETessellationParams::Attachment::makeColor(colorAttachment->color));
            }

            _tessalatePriv(middle_rect_params,frontFaceRotation,viewport,result);
            /// Top Right Arc
            tessellateArc(GPoint2D {object.w - rad_x, object.h - rad_y}, rad_x, rad_y, float(PI) / 2.f, 0, -arcStep);

            /// Right Rect
            middle_rect = GRect {GPoint2D{object.w - rad_x,rad_y},rad_x,object.h - (2 * rad_y)};
            middle_rect_params = TETessellationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETessellationParams::Attachment::makeColor(colorAttachment->color));
            }

            _tessalatePriv(middle_rect_params,frontFaceRotation,viewport,result);

            /// Bottom Right Arc
            tessellateArc(GPoint2D {object.w - rad_x, rad_y}, rad_x, rad_y, 0, -float(PI) / 2.f, -arcStep);

            /// Bottom Rect
            middle_rect = GRect {GPoint2D{rad_x,0.f},object.w - (rad_x * 2),rad_y};
            middle_rect_params = TETessellationParams::Rect(middle_rect);
            if(colorAttachment){
                middle_rect_params.addAttachment(
                        TETessellationParams::Attachment::makeColor(colorAttachment->color));
            }
            _tessalatePriv(middle_rect_params,frontFaceRotation,viewport,result);

            break;
        }
        case TETessellationParams::TESSALATE_ELLIPSOID : {
            auto & object = params.params->ellipsoid;

            TETessellationResult::TEMesh mesh {TETessellationResult::TEMesh::TopologyTriangle};
            std::optional<TETessellationResult::AttachmentData> colorAttachment;
            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETessellationParams::Attachment::TypeColor){
                    colorAttachment = std::make_optional<TETessellationResult::AttachmentData>(
                            TETessellationResult::AttachmentData{attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
                }
            }

            float centerX,centerY,centerZ;
            translateCoords(object.x,object.y,object.z,viewport,&centerX,&centerY,&centerZ);

            auto makePoint = [&](float angle){
                float x = object.x + std::cosf(angle) * object.rad_x;
                float y = object.y + std::sinf(angle) * object.rad_y;
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

                TETessellationResult::TEMesh::Polygon tri {};
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
        case TETessellationParams::TESSELLATE_RECTANGULAR_PRISM : {
            auto & object = params.params->prism;


            TETessellationResult::TEMesh mesh {TETessellationResult::TEMesh::TopologyTriangle};
            TETessellationResult::TEMesh::Polygon tri {};

            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETessellationParams::Attachment::TypeColor) {
                    tri.a.attachment = tri.b.attachment = tri.c.attachment = std::make_optional<TETessellationResult::AttachmentData>(
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
        case TETessellationParams::TESSALATE_GRAPHICSPATH2D : {
            if(params.graphicsPath2D == nullptr){
                break;
            }
            auto & path = *params.graphicsPath2D;

            TETessellationResult::TEMesh mesh {TETessellationResult::TEMesh::TopologyTriangle};
            std::optional<TETessellationResult::AttachmentData> colorAttachment;
            if(!params.attachments.empty()){
                auto & attachment = params.attachments.front();
                if(attachment.type == TETessellationParams::Attachment::TypeColor){
                    colorAttachment = std::make_optional<TETessellationResult::AttachmentData>(
                            TETessellationResult::AttachmentData{attachment.colorData.color,FVec<2>::Create(),FVec<3>::Create()});
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

                TETessellationResult::TEMesh::Polygon p {};
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
        default: {
            break;
        }
    }

};

SharedHandle<OmegaTessellationEngineContext> CreateNativeRenderTargetTEContext(SharedHandle<GENativeRenderTarget> & renderTarget);
SharedHandle<OmegaTessellationEngineContext> CreateTextureRenderTargetTEContext(SharedHandle<GETextureRenderTarget> & renderTarget);

SharedHandle<OmegaTessellationEngineContext> OmegaTessellationEngine::createTEContextFromNativeRenderTarget(SharedHandle<GENativeRenderTarget> & renderTarget){
    return CreateNativeRenderTargetTEContext(renderTarget);
};

SharedHandle<OmegaTessellationEngineContext> OmegaTessellationEngine::createTEContextFromTextureRenderTarget(SharedHandle<GETextureRenderTarget> & renderTarget){
    return CreateTextureRenderTargetTEContext(renderTarget);
};

std::future<TETessellationResult> OmegaTessellationEngineContext::tessalateAsync(const TETessellationParams &params,GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport){
    std::promise<TETessellationResult> prom;
    auto fut = prom.get_future();
    TETessellationParams paramsCopy = params;
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
            promise.set_value_at_thread_exit(tessalateSync(paramsCopy,frontFaceRotation,viewportPtr));
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

TETessellationResult OmegaTessellationEngineContext::tessalateSync(const TETessellationParams &params,GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport){
    TETessellationResult res;
    _tessalatePriv(params,frontFaceRotation,viewport,res);
    return res;
};

OmegaTessellationEngineContext::~OmegaTessellationEngineContext(){
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

void TETessellationResult::translate(float x,float y,float z,const GEViewport & viewport){
    for(auto & m : meshes){
        m.translate(x,y,z,viewport);
    }
};

void TETessellationResult::rotate(float pitch,float yaw,float roll){
    for(auto & m : meshes){
        m.rotate(pitch,yaw,roll);
    }
};

void TETessellationResult::scale(float w,float h,float l){
    for(auto & m : meshes){
        m.scale(w,h,l);
    }
};

void TETessellationResult::TEMesh::translate(float x, float y, float z,const GEViewport & viewport) {
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

void TETessellationResult::TEMesh::rotate(float pitch, float yaw, float roll) {

    /// Pitch Rotation -- X Axis.
    /// Yaw Rotation -- Y Axis.
    /// Roll Rotation -- Z Axis.

    auto cos_pitch = cosf(pitch),sin_pitch = sinf(pitch);
    auto cos_yaw = cosf(yaw),sin_yaw =sinf(yaw);
    auto cos_roll = cosf(roll),sin_roll = sinf(roll);

    auto rotatePoint = [&](GPoint3D & pt){
        /// Pitch Rotation
        pt.x *= 1;
        pt.z = (0 + (cos_pitch * pt.z) - (sin_pitch * pt.y));
        pt.y = (0 + (sin_pitch * pt.z) + (cos_pitch * pt.y));
        /// Yaw Rotation
        pt.y *= 1;
        pt.x = (0 + (cos_yaw * pt.x) - (sin_yaw * pt.z));
        pt.z = (0 + (sin_yaw * pt.x) + (cos_yaw * pt.z));
        /// Roll Rotation
        pt.z *= 1;
        pt.x = (0 + (cos_roll * pt.x) - (sin_roll * pt.y));
        pt.y = (0 + (sin_roll * pt.x) + (cos_roll * pt.y));
    };

    for(auto & polygon : vertexPolygons){
        rotatePoint(polygon.a.pt);
        rotatePoint(polygon.b.pt);
        rotatePoint(polygon.c.pt);
    }
}

void TETessellationResult::TEMesh::scale(float w, float h,float l) {
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
