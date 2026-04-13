#include "GTEBase.h"
#include <thread>
#include <future>
#include <mutex>
#include <optional> 
#include <type_traits>
#include "GE.h"
#include "GETexture.h"


#ifndef OMEGAGTE_TE_H
#define OMEGAGTE_TE_H

_NAMESPACE_BEGIN_
/**
 *
 Defines the arguments for the triangulation operations.
*/
struct OMEGAGTE_EXPORT TETriangulationParams {
    private:
    struct GraphicsPath3DParams;
    typedef enum : unsigned char {
        TRIANGULATE_RECT,
        TRIANGULATE_ROUNDEDRECT,
        TRIANGULATE_RECTANGULAR_PRISM,
        TRIANGULATE_PYRAMID,
        TRIANGULATE_ELLIPSOID,
        TRIANGULATE_CYLINDER,
        TRIANGULATE_CONE,
        TRIANGULATE_GRAPHICSPATH2D,
        TRIANGULATE_GRAPHICSPATH3D
    } TriangulationType;
    TriangulationType type;

    union Data;

    struct DataDelete {
        TriangulationType t;
    public:
        void operator()(Data *ptr);
    };

    std::shared_ptr<Data> params;
    std::shared_ptr<GVectorPath2D> graphicsPath2D;
    bool graphicsPath2DContour = false;
    bool graphicsPath2DFill = false;
    float graphicsPath2DStrokeWidth = 1.f;

    friend class OmegaTriangulationEngineContext;
public:
    struct OMEGAGTE_EXPORT Attachment {
        typedef enum : int {
            TypeColor,
            TypeTexture2D,
            TypeTexture3D
        } Type;
        Type type;
        union {
            struct {
                FVec<4> color = FVec<4>::Create();
            } colorData;
            struct {
                unsigned width;
                unsigned height;
            } texture2DData;
            struct {
                unsigned width;
                unsigned height;
                unsigned depth;
            } texture3DData;
        };
        SharedHandle<GETexture> texture = nullptr;


        static Attachment makeColor(const FVec<4> & color);
        static Attachment makeTexture2D(unsigned width,unsigned height,SharedHandle<GETexture> texture = nullptr);
        static Attachment makeTexture3D(unsigned width,unsigned height,unsigned depth,SharedHandle<GETexture> texture = nullptr);
        Attachment() : type(TypeColor) {}
        Attachment(Type t) : type(t) {}
        Attachment(const Attachment & other);
        Attachment & operator=(const Attachment & other);
        ~Attachment();
    };

    void addAttachment(const Attachment &attachment);
private:
    OmegaCommon::Vector<Attachment> attachments;
public:
  
    /**
      Triangulate a GRect
      @param[in] rect
      @returns TETriangulationParams
    */
    static TETriangulationParams Rect(GRect & rect);

    /**
      Triangulate a GRoundedRect
      @param[in] roundedRect
      @returns TETriangulationParams
    */
    static TETriangulationParams RoundedRect(GRoundedRect & roundedRect);

    /**
      Triangulate a GRectangularPrism
      @param[in] rectPrism
      @returns TETriangulationParams
    */
    static TETriangulationParams RectangularPrism(GRectangularPrism &rectPrism);

    /**
      Triangulate a GPyramid
      @param[in] pyramid
      @returns TETriangulationParams
    */
    static TETriangulationParams Pyramid(GPyramid &pyramid);

    /**
      Triangulate a GRect
      @param[in] rect
      @returns TETriangulationParams
    */
    static TETriangulationParams Ellipsoid(GEllipsoid & ellipsoid);

    /**
      Triangulate a GCylinder
      @param[in] cylinder
      @returns TETriangulationParams
    */
    static TETriangulationParams Cylinder(GCylinder &cylinder);

    /**
      Triangulate a GCone
      @param[in] cone
      @returns TETriangulationParams
    */
    static TETriangulationParams Cone(GCone &cone);

    /**
      Triangulate 2D vector paths
      @param[in] vectorPaths A small array with *only* 2 GVectorPath2D objects.
      @returns TETriangulationParams
    */
    static TETriangulationParams GraphicsPath2D(GVectorPath2D & path,float strokeWidth = 1.f,bool contour = false,bool fill = false);
    /**
      Triangulate 3D vector paths
      @param[in] vectorPathCount The number of vectorPathes to triangulate
      @param[in] vectorPaths An array of GVectorPath3D objects. (Ensure that it has the same length as the `vectorPathCount`)
      @returns TETriangulationParams
    */
    static TETriangulationParams GraphicsPath3D(unsigned vectorPathCount,GVectorPath3D * const vectorPaths);

    ~TETriangulationParams();
};

/**
 A small struct for holding one (or more) 
 meshes that result from a triangulation operation.
*/
struct OMEGAGTE_EXPORT TETriangulationResult {
    struct OMEGAGTE_EXPORT AttachmentData {
        FVec<4> color;
        FVec<2> texture2Dcoord;
        FVec<3> texture3Dcoord;
        FVec<3> normal = FVec<3>::Create();
    };
    struct OMEGAGTE_EXPORT TEMesh {
        enum : int {
            TopologyTriangle,
            TopologyTriangleStrip
        } topology;
        struct OMEGAGTE_EXPORT Polygon {
            struct {
                GPoint3D pt;
                std::optional<AttachmentData> attachment;
            } a,b,c;
        };
        std::vector<Polygon> vertexPolygons;
        void translate(float x,float y,float z,const GEViewport & viewport);
        void rotate(float pitch,float yaw,float roll);
        void scale(float w,float h,float l);
        unsigned vertexCount();
    };
    std::vector<TEMesh> meshes;
    unsigned totalVertexCount();
    void translate(float x,float y,float z,const GEViewport & viewport);
    void rotate(float pitch,float yaw,float roll);
    void scale(float w,float h,float l);
};
/**
 
*/
class OMEGAGTE_EXPORT OmegaTriangulationEngineContext {
    friend class OmegaTriangulationEngine;
protected:
    GEViewport defaultViewport;
    std::vector<std::thread> activeThreads;
    std::mutex activeThreadsMutex;

    float arcStep = 0.01;

    void translateCoordsDefaultImpl(float x, float y,float z,GEViewport * viewport, float *x_result, float *y_result,float *z_result);
    virtual void translateCoords(float x, float y,float z,GEViewport * viewport, float *x_result, float *y_result,float *z_result) = 0;
    virtual GEViewport getEffectiveViewport();
    inline void _triangulatePriv(const TETriangulationParams & params,GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport,TETriangulationResult & result);

public:
    struct GPUTriangulationExtractedParams {
        enum Type { Rect, RoundedRect, Ellipsoid, RectPrism, Path2D, Other } type = Other;
        float rx, ry, rw, rh;
        float ex, ey, erad_x, erad_y;
        float px, py, pz, pw, ph, pd;
        float cr, cg, cb, ca;
        bool hasColor = false;
        float strokeWidth = 1.f;
        bool contour = false;
        struct Segment { float sx, sy, ex, ey; };
        std::vector<Segment> pathSegments;
    };
    void extractGPUTriangulationParams(const TETriangulationParams &params, GPUTriangulationExtractedParams &out);
    OMEGACOMMON_CLASS("OmegaGTE.OmegaTriangulationEngineContext")
    // Default Value: 0.01 radians.
    void setArcStep(float newArcStep){
        arcStep = newArcStep;
    };
    ~OmegaTriangulationEngineContext();
    /**
     Triangulate according to the parameters and viewport.
     @param params
      @param frontFaceRotation
     @param viewport
     @returns TETriangulationResult
    */
    TETriangulationResult triangulateSync(const TETriangulationParams & params, GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise,GEViewport * viewport = nullptr);

    /**
      Performs triangulation like `triangulateSync` (@see triangulateSync), 
      however it performs the computation in a compute pipeline.
     @param params
      @param frontFaceRotation
     @param viewport
     @returns std::future<TETriangulationResult>
    */
    virtual std::future<TETriangulationResult> triangulateOnGPU(const TETriangulationParams & params,GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise, GEViewport * viewport = nullptr) = 0;

    /**
      Performs triangulation like `triangulateSync` (@see triangulateSync), 
      however it performs the computation in a seperate thread
     @param params
     @param frontFaceRotation
     @param viewport
     @returns std::future<TETriangulationResult>
    */
    std::future<TETriangulationResult> triangulateAsync(const TETriangulationParams & params,GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise, GEViewport * viewport = nullptr);

};

/**
 @brief The Omega Triangulation Engine
*/
class OMEGAGTE_EXPORT OmegaTriangulationEngine {
public:
    OMEGACOMMON_CLASS("OmegaGTE.OmegaTriangulationEngine")
  /**
   NEVER CALL THIS FUNCTION! Please invoke GTE::Init()
  */
  static SharedHandle<OmegaTriangulationEngine> Create();
    /**
        Create a Triangulation Context from a GENativeRenderTarget
        @param[in] renderTarget
        @returns SharedHandle<OmegaTriangulationEngineContext>
    */
    SharedHandle<OmegaTriangulationEngineContext> createTEContextFromNativeRenderTarget(SharedHandle<GENativeRenderTarget> & renderTarget);

    /**
        Create a Triangulation Context from a GETextureRenderTarget
        @param[in] renderTarget
        @returns SharedHandle<OmegaTriangulationEngineContext>
    */
    SharedHandle<OmegaTriangulationEngineContext> createTEContextFromTextureRenderTarget(SharedHandle<GETextureRenderTarget> & renderTarget);
};

_NAMESPACE_END_

#endif
