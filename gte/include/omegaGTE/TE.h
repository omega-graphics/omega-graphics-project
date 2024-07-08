#include "GTEBase.h"
#include <thread>
#include <future>
#include <optional> 
#include <type_traits>
#include "GE.h"


#ifndef OMEGAGTE_TE_H
#define OMEGAGTE_TE_H

_NAMESPACE_BEGIN_
/**
 *
 Defines the arguments for the tessellation operations.
*/
struct OMEGAGTE_EXPORT TETessellationParams {
    private:
    struct GraphicsPath2DParams;
    struct GraphicsPath3DParams;
    typedef enum : unsigned char {
        TESSALATE_RECT,
        TESSALATE_ROUNDEDRECT,
        TESSELLATE_RECTANGULAR_PRISM,
        TESSALATE_PYRAMID,
        TESSALATE_ELLIPSOID,
        TESSALATE_CYLINDER,
        TESSALATE_CONE,
        TESSALATE_GRAPHICSPATH2D,
        TESSALATE_GRAPHICSPATH3D
    } TessalationType;
    TessalationType type;

    union Data;

    struct DataDelete {
        TessalationType t;
    public:
        void operator()(Data *ptr);
    };

    std::unique_ptr<Data,DataDelete> params;

    friend class OmegaTessellationEngineContext;
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


        static Attachment makeColor(const FVec<4> & color);
        static Attachment makeTexture2D(unsigned width,unsigned height);
        static Attachment makeTexture3D(unsigned width,unsigned height,unsigned depth);
        ~Attachment();
    };

    void addAttachment(const Attachment &attachment);
private:
    OmegaCommon::Vector<Attachment> attachments;
public:
  
    /**
      Tessalate a GRect
      @param[in] rect 
      @returns TETessalationParams
    */
    static std::add_rvalue_reference_t<TETessellationParams> Rect(GRect & rect);

    /**
      Tessalate a GRoundedRect
      @param[in] roundedRect 
      @returns TETessalationParams
    */
    static std::add_rvalue_reference_t<TETessellationParams> RoundedRect(GRoundedRect & roundedRect);

    /**
      Tessalate a GRectangularPrism
      @param[in] rectPrism 
      @returns TETessalationParams
    */
    static std::add_rvalue_reference_t<TETessellationParams> RectangularPrism(GRectangularPrism &rectPrism);

    /**
      Tessalate a GPyramid
      @param[in] pyramid 
      @returns TETessalationParams
    */
    static std::add_rvalue_reference_t<TETessellationParams> Pyramid(GPyramid &pyramid);

    /**
      Tessalate a GRect
      @param[in] rect 
      @returns TETessalationParams
    */
    static std::add_rvalue_reference_t<TETessellationParams> Ellipsoid(GEllipsoid & ellipsoid);

    /**
      Tessalate a GCylinder
      @param[in] cylinder 
      @returns TETessalationParams
    */
    static std::add_rvalue_reference_t<TETessellationParams> Cylinder(GCylinder &cylinder);

    /**
      Tessalate a GCone
      @param[in] cone 
      @returns TETessalationParams
    */
    static std::add_rvalue_reference_t<TETessellationParams> Cone(GCone &cone);

    /**
      Tessalate 2D vector paths
      @param[in] vectorPaths A small array with *only* 2 GVectorPath2D objects.
      @returns TETessalationParams
    */
    static std::add_rvalue_reference_t<TETessellationParams> GraphicsPath2D(GVectorPath2D & path,float strokeWidth = 1.f,bool contour = false,bool fill = false);
    /**
      Tessalate 3D vector paths
      @param[in] vectorPathCount The number of vectorPathes to tessalate 
      @param[in] vectorPaths An array of GVectorPath3D objects. (Ensure that it has the same length as the `vectorPathCount`)
      @returns TETessalationParams
    */
    static std::add_rvalue_reference_t<TETessellationParams> GraphicsPath3D(unsigned vectorPathCount,GVectorPath3D * const vectorPaths);

    ~TETessellationParams();
};

/**
 A small struct for holding one (or more) 
 meshes that result from a tesslation operation.
*/
struct OMEGAGTE_EXPORT TETessellationResult {
    struct OMEGAGTE_EXPORT AttachmentData {
        FVec<4> color;
        FVec<2> texture2Dcoord;
        FVec<3> texture3Dcoord;
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
class OMEGAGTE_EXPORT OmegaTessellationEngineContext {
    friend class OmegaTessellationEngine;
protected:
    GEViewport defaultViewport;
    std::vector<std::thread *> activeThreads;

    float arcStep = 0.01;

    void translateCoordsDefaultImpl(float x, float y,float z,GEViewport * viewport, float *x_result, float *y_result,float *z_result);
    virtual void translateCoords(float x, float y,float z,GEViewport * viewport, float *x_result, float *y_result,float *z_result) = 0;
    inline void _tessalatePriv(const TETessellationParams & params,GTEPolygonFrontFaceRotation frontFaceRotation, GEViewport * viewport,TETessellationResult & result);
public:
    OMEGACOMMON_CLASS("OmegaGTE.OmegaTessellationEngineContext")
    // Default Value: 0.01 radians.
    void setArcStep(float newArcStep){
        arcStep = newArcStep;
    };
    ~OmegaTessellationEngineContext();
    /**
     Tessalate according to the parameters and viewport.
     @param params
      @param frontFaceRotation
     @param viewport
     @returns TETessellationResult
    */
    TETessellationResult tessalateSync(const TETessellationParams & params, GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise,GEViewport * viewport = nullptr);

    /**
      Performs tessalation like `tessalateSync` (@see tessalateSync), 
      however it performs the computation in a compute pipeline.
     @param params
      @param frontFaceRotation
     @param viewport
     @returns std::future<TETessellationResult>
    */
    virtual std::future<TETessellationResult> tessalateOnGPU(const TETessellationParams & params,GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise, GEViewport * viewport = nullptr) = 0;

    /**
      Performs tessalation like `tessalateSync` (@see tessalateSync), 
      however it performs the computation in a seperate thread
     @param params
     @param frontFaceRotation
     @param viewport
     @returns std::future<TETessellationResult>
    */
    std::future<TETessellationResult> tessalateAsync(const TETessellationParams & params,GTEPolygonFrontFaceRotation frontFaceRotation = GTEPolygonFrontFaceRotation::Clockwise, GEViewport * viewport = nullptr);

};

/**
 @brief The Omega Tessalation Engine
*/
class OMEGAGTE_EXPORT OmegaTessellationEngine {
public:
    OMEGACOMMON_CLASS("OmegaGTE.OmegaTessellationEngine")
  /**
   NEVER CALL THIS FUNCTION! Please invoke GTE::Init()
  */
  static SharedHandle<OmegaTessellationEngine> Create();
    /**
        Create a Tessalation Context from a GENativeRenderTarget
        @param[in] renderTarget
        @returns SharedHandle<OmegaTessellationEngineContext>
    */
    SharedHandle<OmegaTessellationEngineContext> createTEContextFromNativeRenderTarget(SharedHandle<GENativeRenderTarget> & renderTarget);

    /**
        Create a Tessalation Context from a GETextureRenderTarget
        @param[in] renderTarget
        @returns SharedHandle<OmegaTessellationEngineContext>
    */
    SharedHandle<OmegaTessellationEngineContext> createTEContextFromTextureRenderTarget(SharedHandle<GETextureRenderTarget> & renderTarget);
};

_NAMESPACE_END_

#endif
