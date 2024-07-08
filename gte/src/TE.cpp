#include "omegaGTE/TE.h"
// #include "omegaGTE/GTEShaderTypes.h"
#include <optional>
#include <thread>
#include <iostream>

_NAMESPACE_BEGIN_


struct TETessellationParams::GraphicsPath2DParams {
    GVectorPath2D path;
    bool treatAsContour;
    bool fill;
    float strokeWidth;
    GraphicsPath2DParams(GVectorPath2D & path,bool treatAsContour,bool fill,float strokeWidth):
    path(path),
    treatAsContour(treatAsContour),
    fill(fill),
    strokeWidth(strokeWidth){

    };
};

struct TETessellationParams::GraphicsPath3DParams {
    GVectorPath3D *const pathes;
    unsigned pathCount;
    GraphicsPath3DParams(GVectorPath3D *const pathes,unsigned pathCount):pathes(pathes),pathCount(pathCount){};
};

union TETessellationParams::Data {

    GRect rect;

    GRoundedRect rounded_rect;
    
    GCone cone;

    GEllipsoid ellipsoid;

    GraphicsPath2DParams path2D;

    GraphicsPath2DParams path3D;

    ~Data();
};

void TETessellationParams::DataDelete::operator()(Data *ptr) {

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

std::add_rvalue_reference_t<TETessellationParams> TETessellationParams::Rect(GRect &rect){
    TETessellationParams params;
    params.type = TESSALATE_RECT;
    params.params.reset(new Data{});
    params.params.get_deleter().t = params.type;
    params.params->rect = rect;
    return std::move(params);
};

std::add_rvalue_reference_t<TETessellationParams> TETessellationParams::RoundedRect(GRoundedRect &roundedRect){
    TETessellationParams params;
    params.type = TESSALATE_ROUNDEDRECT;
    params.params.reset(new Data{});
    params.params.get_deleter().t = params.type;
    params.params->rounded_rect = roundedRect;
    return std::move(params);
};

std::add_rvalue_reference_t<TETessellationParams> TETessellationParams::RectangularPrism(GRectangularPrism &prism){
    TETessellationParams params;
    params.params.reset(new Data{});
    params.params.get_deleter().t = params.type;
    params.type = TESSELLATE_RECTANGULAR_PRISM;
    return std::move(params);
};

std::add_rvalue_reference_t<TETessellationParams> TETessellationParams::Pyramid(GPyramid &pyramid){
    TETessellationParams params;
    params.params = &pyramid;
    params.type = TESSALATE_PYRAMID;
    return params;
};

std::add_rvalue_reference_t<TETessellationParams> TETessellationParams::Ellipsoid(GEllipsoid &ellipsoid){
    TETessellationParams params;
    params.params = &ellipsoid;
    params.type = TESSALATE_ELLIPSOID;
    return params;
};

std::add_rvalue_reference_t<TETessellationParams> TETessellationParams::Cylinder(GCylinder &cylinder){
    TETessellationParams params;
    params.params = &cylinder;
    params.type = TESSALATE_CYLINDER;
    return params;
};

std::add_rvalue_reference_t<TETessellationParams> TETessellationParams::Cone(GCone &cone){
    TETessellationParams params;
    params.params = &cone;
    params.type = TESSALATE_CONE;
    return params;
};

std::add_rvalue_reference_t<TETessellationParams> TETessellationParams::GraphicsPath2D(GVectorPath2D & path,float strokeWidth,bool contour,bool fill){
    TETessellationParams params;
    auto * _params = new GraphicsPath2DParams(path,contour,fill,strokeWidth);
    params.params = _params;
    params.type = TESSALATE_GRAPHICSPATH2D;
    return params;
};

std::add_rvalue_reference_t<TETessellationParams> TETessellationParams::GraphicsPath3D(unsigned int vectorPathCount, GVectorPath3D *const vectorPaths){
    TETessellationParams params;
    GraphicsPath3DParams * _params = new GraphicsPath3DParams(vectorPaths,vectorPathCount);
    params.params = _params;
    params.type = TESSALATE_GRAPHICSPATH3D;
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
            GRect *object = (GRect *)params.params;

            TETessellationResult::TEMesh mesh {TETessellationResult::TEMesh::TopologyTriangle};
            TETessellationResult::TEMesh::Polygon tri {};
            float x0,x1,y0,y1;
            float u,v;
            translateCoords(object->pos.x,object->pos.y,0.f,viewport,&x0,&y0,nullptr);
            translateCoords(object->pos.x + object->w,object->pos.y + object->h,0.f,viewport,&x1,&y1,nullptr);

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
            auto object = (GRoundedRect *)params.params;

            GRect middle_rect {object->rad_x,object->rad_y,object->w - (2 * object->rad_x),object->h - (2 * object->rad_y)};

            auto middle_rect_params = TETessellationParams::Rect(middle_rect);

            _tessalatePriv(middle_rect_params,frontFaceRotation,viewport,result);

            auto tessellateArc = [&](GPoint2D start, float rad_x, float rad_y, float angle_start, float angle_end, float _arcStep){
                TETessellationResult::TEMesh m {TETessellationResult::TEMesh::TopologyTriangleStrip};
                GPoint3D pt_a {start.x,start.y,0.f};
                auto _angle_it = angle_start;
                while(_angle_it <= angle_end){
                    TETessellationResult::TEMesh::Polygon p {};

                    auto x_f = std::cosf(_angle_it) * rad_x;
                    auto y_f = std::sinf(_angle_it) * rad_y;

                    x_f += start.x;
                    y_f += start.y;

                    p.a.pt = pt_a;
                    p.b.pt = GPoint3D {x_f,y_f,0.f};


                    _angle_it += _arcStep;

                    x_f = std::cosf(_angle_it) * rad_x;
                    y_f = std::sinf(_angle_it) * rad_y;

                    x_f += start.x;
                    y_f += start.y;

                    p.c.pt = GPoint3D {x_f,y_f,0.f};

                    m.vertexPolygons.push_back(p);
                }
                result.meshes.push_back(m);
            };

            /// Bottom Left Arc
            tessellateArc(GPoint2D {object->rad_x, object->rad_y}, object->rad_x, object->rad_y, float(3.f * PI) / 2.f, PI, -arcStep);

            /// Left Rect
            middle_rect = GRect {GPoint2D{0.f,object->rad_y},object->rad_x,object->h - (2 * object->rad_y)};
            middle_rect_params = TETessellationParams::Rect(middle_rect);

            _tessalatePriv(middle_rect_params,frontFaceRotation,viewport,result);
            /// Top Left Arc
            tessellateArc(GPoint2D {object->rad_x, object->h - object->rad_y}, object->rad_x, object->rad_y, PI, float(PI) / 2.f, -arcStep);

            /// Top Rect
            middle_rect = GRect {GPoint2D{object->rad_x,object->h - object->rad_y},object->w - (object->rad_x * 2),object->rad_y};
            middle_rect_params = TETessellationParams::Rect(middle_rect);

            _tessalatePriv(middle_rect_params,frontFaceRotation,viewport,result);
            /// Top Right Arc
            tessellateArc(GPoint2D {object->w - object->rad_x, object->h - (object->rad_y)}, object->rad_x, object->rad_y, float(PI) / 2.f, 0, -arcStep);

            /// Right Rect
            middle_rect = GRect {GPoint2D{object->w - object->rad_x,object->rad_y},object->rad_x,object->h - (2 * object->rad_y)};
            middle_rect_params = TETessellationParams::Rect(middle_rect);

            _tessalatePriv(middle_rect_params,frontFaceRotation,viewport,result);

            /// Bottom Right Arc
            tessellateArc(GPoint2D {object->w - object->rad_x, object->rad_y}, object->rad_x, object->rad_y, 0, -float(PI) / 2.f, -arcStep);

            /// Bottom Rect
            middle_rect = GRect {GPoint2D{0.f,0.f},object->w - (object->rad_x * 2),object->rad_y};
            middle_rect_params = TETessellationParams::Rect(middle_rect);

            break;
        }
        case TETessellationParams::TESSELLATE_RECTANGULAR_PRISM : {
            auto object = (GRectangularPrism *)params.params;


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
            translateCoords(object->pos.x,object->pos.y,object->pos.z,viewport,&x0,&y0,&z0);
            translateCoords(object->pos.x + object->w,
                            object->pos.y + object->h,
                            object->pos.z + object->d,
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
            auto object = (TETessellationParams::GraphicsPath2DParams *)params.params;

            TETessellationResult::TEMesh mesh {TETessellationResult::TEMesh::TopologyTriangle};

            float deviceCoordStrokeWidthX = object->strokeWidth/(2 * viewport->width);
            float deviceCoordStrokeWidthY = object->strokeWidth/(2 * viewport->height);

            TETessellationResult::TEMesh::Polygon polygon{};
            /// 1. Triangulate Stroke of Path.

            GVectorPath2D path_a{GPoint2D {0,0}},path_b{GPoint2D{0,0}};

            for(auto path_it = object->path.begin();path_it != object->path.end();path_it.operator++()){
                auto segment = *path_it;
                auto tan_m =  -(segment.pt_B->x - segment.pt_A->x)/(segment.pt_B->y - segment.pt_A->y);
                auto tan_m_sq = tan_m * tan_m;
                auto cos_m_sq = 1.f/(1 + (tan_m_sq));
                auto cos_m = std::sqrt(cos_m_sq);
                float stroke_x = deviceCoordStrokeWidthX * std::sqrt(cos_m_sq);
                float stroke_y = deviceCoordStrokeWidthY * (cos_m * tan_m);

                auto pt_a = GPoint3D {segment.pt_A->x + stroke_x,segment.pt_A->y + stroke_y,0.f};
                auto pt_b = GPoint3D {segment.pt_A->x - stroke_x,segment.pt_A->y - stroke_y,0.f};

                if(path_a.size() == 0){
                    auto & pt = path_a.firstPt();
                    pt.x = pt_a.x;
                    pt.y = pt_a.y;

                    pt = path_b.firstPt();
                    pt.x = pt_b.x;
                    pt.y = pt_b.y;

                }

                auto pt_c = GPoint3D {segment.pt_B->x + stroke_x,segment.pt_B->y + stroke_y,0.f};
                auto pt_d = GPoint3D {segment.pt_B->x - stroke_x,segment.pt_B->y - stroke_y,0.f};

                path_a.append(GPoint2D {pt_c.x,pt_c.y});
                path_b.append(GPoint2D {pt_d.x,pt_d.y});

                polygon.a.pt = pt_a;
                polygon.b.pt = pt_b;
                polygon.c.pt = pt_c;

                mesh.vertexPolygons.push_back(polygon);

                polygon.a.pt = pt_c;
                polygon.b.pt = pt_b;
                polygon.c.pt = pt_d;

                mesh.vertexPolygons.push_back(polygon);
            }

            /// 2. If it is a contour, close the path.

            if(object->treatAsContour){
                auto & start_pt = object->path.firstPt();
                auto & end_pt = object->path.lastPt();

                auto tan_m = -(end_pt.x - start_pt.x)/(end_pt.y - start_pt.y);
                auto tan_m_sq = tan_m * tan_m;
                auto cos_m_sq = 1.f/(1 + (tan_m_sq));
                auto cos_m = std::sqrt(cos_m_sq);
                float stroke_x = deviceCoordStrokeWidthX * std::sqrt(cos_m_sq);
                float stroke_y = deviceCoordStrokeWidthY * (cos_m * tan_m);

                auto pt_a = GPoint3D {start_pt.x + stroke_x,start_pt.y + stroke_y,0.f};
                auto pt_b = GPoint3D {start_pt.x- stroke_x,start_pt.y - stroke_y,0.f};

                auto pt_c = GPoint3D {end_pt.x + stroke_x,end_pt.y + stroke_y,0.f};
                auto pt_d = GPoint3D {end_pt.x - stroke_x,end_pt.y - stroke_y,0.f};

                polygon.a.pt = pt_a;
                polygon.b.pt = pt_b;
                polygon.c.pt = pt_c;

                mesh.vertexPolygons.push_back(polygon);

                polygon.a.pt = pt_c;
                polygon.b.pt = pt_b;
                polygon.c.pt = pt_d;

                mesh.vertexPolygons.push_back(polygon);

                /// 3. Fill the contour if needed.

                if(object->fill){
                    GVectorPath2D *inner_path;
                    if(object->strokeWidth > 0){
                        if(path_a.mag() > path_b.mag()){
                            inner_path = &path_b;
                        }
                        else {
                            inner_path = &path_a;
                        }
                    }
                    else {
                        inner_path = &object->path;
                    }
                    
                    if(inner_path->size() > 2) {

                        GVectorPath2D trace_path{*inner_path};
                        GVectorPath2D trace_path__t{inner_path->firstPt()};
                        OmegaCommon::Vector<std::pair<GPoint2D,float>> trace_path_record;
                        TETessellationResult::TEMesh fill_mesh;
                        fill_mesh.topology = TETessellationResult::TEMesh::TopologyTriangle;

                        while (trace_path.size() > 3) {
                            float cross_result = 0;

                            /// 1. Calculate Fill Direction of Polygon 
                            for (auto path_it = trace_path.begin(); path_it != trace_path.end(); path_it.operator++()) {
                                GPoint2D a, b, c;

                                auto seg = *path_it;
                                a = *seg.pt_A;
                                b = *seg.pt_B;
                                path_it.operator++();
                                if (path_it == trace_path.end()) {
                                    c = trace_path.firstPt();
                                } else {
                                    c = *(*path_it).pt_B;
                                }
                                FVector3D vec_a{b.x - a.x, b.y - a.y, 0.f}, vec_b{c.x - b.x, c.y - b.y, 0.f};
                                auto res = vec_a.cross(vec_b).getK();
                                trace_path_record.push_back(std::make_pair(b,res));
                                cross_result += res;
                            }

                            #define NEXT_ON_PATH(p) ++(p); if(p == trace_path_record.end()){ p = trace_path_record.begin();};

                            /// 2. Fill Polygon recursively,
                            bool plus_dominant = cross_result > 0,minus_dominant = cross_result < 0;

                            for(auto pt_it = trace_path_record.begin(); pt_it != trace_path_record.end();){
                                auto first_pt = pt_it->first;
                                TETessellationResult::TEMesh::Polygon polygon;
                                // polygon.a.attachment = polygon.b.attachment = polygon.c.attachment = std::make_optional(TETessellationResult::AttachmentData{}); 

                                NEXT_ON_PATH(pt_it);

                                if(plus_dominant){
                                    if(pt_it->second > 0){
                                        polygon.a.pt = GPoint3D{first_pt.x,first_pt.y,0.f};
                                        polygon.b.pt = GPoint3D{pt_it->first.x,pt_it->first.y,0.f};

                                        NEXT_ON_PATH(pt_it);

                                        polygon.c.pt = GPoint3D{pt_it->first.x,pt_it->first.y,0.f};
                                        fill_mesh.vertexPolygons.push_back(polygon);

                                        trace_path__t.append(first_pt);
                                        trace_path__t.append(pt_it->first);
                                    }
                                    else {
                                         trace_path__t.append(first_pt);
                                    }
                                }
                                else if(minus_dominant){
                                    if(pt_it->second < 0){
                                        polygon.a.pt = GPoint3D{first_pt.x,first_pt.y,0.f};
                                        polygon.b.pt = GPoint3D{pt_it->first.x,pt_it->first.y,0.f};

                                        NEXT_ON_PATH(pt_it);

                                        polygon.c.pt = GPoint3D{pt_it->first.x,pt_it->first.y,0.f};
                                        fill_mesh.vertexPolygons.push_back(polygon);

                                        trace_path__t.append(first_pt);
                                        trace_path__t.append(pt_it->first);
                                    }
                                    else {
                                         trace_path__t.append(first_pt);
                                    }
                                }
                                else {
                                    trace_path__t.append(first_pt);
                                    trace_path__t.append(pt_it->first);
                                     NEXT_ON_PATH(pt_it);
                                     trace_path__t.append(pt_it->first);
                                }

                            
                            }

                            trace_path = trace_path__t;
                            trace_path__t.reset(trace_path__t.lastPt());
                        }
                        TETessellationResult::TEMesh::Polygon polygon;
                        auto it = trace_path.begin();
                        auto seg0 = *it;
                        ++it;
                        auto seg1 = *it;

                        polygon.a.pt = GPoint3D{seg0.pt_A->x,seg0.pt_A->y,0.f};
                        polygon.b.pt = GPoint3D{seg0.pt_B->x,seg0.pt_B->y,0.f};
                        polygon.c.pt = GPoint3D{seg0.pt_B->x,seg0.pt_B->y,0.f};
                        
                        fill_mesh.vertexPolygons.push_back(polygon);
                        result.meshes.push_back(fill_mesh);
                    }
                    
                }

            }



            /// Finish
            result.meshes.push_back(mesh);
            

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
    activeThreads.emplace_back(new std::thread([&](std::promise<TETessellationResult> promise, size_t idx){
        promise.set_value_at_thread_exit(tessalateSync(params,frontFaceRotation,viewport));
        activeThreads.erase(activeThreads.begin() + idx);
    },std::move(prom),activeThreads.size()));
    return fut;
};

TETessellationResult OmegaTessellationEngineContext::tessalateSync(const TETessellationParams &params,GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport){
    TETessellationResult res;
    _tessalatePriv(params,frontFaceRotation,viewport,res);
    return res;
};

OmegaTessellationEngineContext::~OmegaTessellationEngineContext(){
    for(auto t : activeThreads){
        if(t->joinable()){
            t->join();
        }
    };
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

    auto cos_pitch = std::cosf(pitch),sin_pitch = std::sinf(pitch);
    auto cos_yaw = std::cosf(yaw),sin_yaw = std::sinf(yaw);
    auto cos_roll = std::cosf(roll),sin_roll = std::sinf(roll);

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
