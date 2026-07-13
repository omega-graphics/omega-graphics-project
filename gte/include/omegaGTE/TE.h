#include "GTEBase.h"
#include "GTEMath.h"
#include <thread>
#include <future>
#include <mutex>
#include <optional>
#include <type_traits>
#include <cstdint>
#include "GE.h"
#include "GETexture.h"


#ifndef OMEGAGTE_TE_H
#define OMEGAGTE_TE_H

_NAMESPACE_BEGIN_

/// @brief How consecutive stroke segments are connected at a path vertex.
enum class StrokeJoin : int {
    Miter,  ///< Extend outer edges until they meet; falls back to Bevel past the miter limit.
    Round,  ///< Arc of triangles between the two segment normals.
    Bevel   ///< Single triangle bridging the outer endpoints.
};

/// @brief How an open path's stroke is terminated at its endpoints.
enum class StrokeCap : int {
    Butt,    ///< No extension beyond the endpoint.
    Round,   ///< Semicircle fan, radius = strokeWidth/2.
    Square   ///< Extends the stroke by strokeWidth/2 as a rectangle.
};

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
        TRIANGULATE_TORUS,
        TRIANGULATE_SPHERE,
        TRIANGULATE_CAPSULE,
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
    StrokeJoin graphicsPath2DJoin = StrokeJoin::Miter;
    StrokeCap graphicsPath2DCap = StrokeCap::Butt;

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
      Triangulate a GTorus
      @param[in] torus
      @returns TETriangulationParams
    */
    static TETriangulationParams Torus(GTorus &torus);

    /**
      Triangulate a GSphere
      @param[in] sphere
      @returns TETriangulationParams
    */
    static TETriangulationParams Sphere(GSphere &sphere);

    /**
      Triangulate a GCapsule
      @param[in] capsule
      @returns TETriangulationParams
    */
    static TETriangulationParams Capsule(GCapsule &capsule);

    /**
      Triangulate a 2D vector path.
      @param[in] path The path to stroke and/or fill.
      @param[in] strokeWidth Stroke width in path units (0 disables the stroke).
      @param[in] contour If true, the stroke closes back to the first point.
      @param[in] fill If true, the interior is filled (uses the second attachment).
      @param[in] join Join style applied at interior vertices.
      @param[in] cap Cap style applied at the endpoints of an open path.
      @returns TETriangulationParams
    */
    static TETriangulationParams GraphicsPath2D(GVectorPath2D & path,float strokeWidth = 1.f,bool contour = false,bool fill = false,StrokeJoin join = StrokeJoin::Miter,StrokeCap cap = StrokeCap::Butt);
    /**
      Triangulate 3D vector paths
      @param[in] vectorPathCount The number of vectorPathes to triangulate
      @param[in] vectorPaths An array of GVectorPath3D objects. (Ensure that it has the same length as the `vectorPathCount`)
      @param[in] strokeWidth Stroke width in path units.
      @returns TETriangulationParams
    */
    static TETriangulationParams GraphicsPath3D(unsigned vectorPathCount,GVectorPath3D * const vectorPaths,float strokeWidth = 1.f);

    /// @brief The coordinate space this geometry is authored in.
    ///
    /// When set, triangulation maps input coordinates into NDC relative to
    /// THIS viewport (origin + extent + depth range) instead of the render
    /// target's effective viewport. Use it for a render pass whose units are
    /// not the render target's pixels — a scene drawn into a sub-region of a
    /// larger target, or one authored in an arbitrary world space.
    ///
    /// The coordinate space is a property of the geometry, not of the call, so
    /// this field **wins over** the loose `viewport` argument on
    /// `triangulateSync` / `triangulateAsync` / `triangulateOnGPU`. Full
    /// precedence: `params.viewport` → call argument → `getEffectiveViewport()`.
    std::optional<GEViewport> viewport;

    /// @brief Emit vertices in the geometry's own units, with no NDC bake.
    ///
    /// Triangulation normally converts every coordinate to NDC against the
    /// resolved viewport, so its output is already in clip space. When this is
    /// true the conversion becomes an identity pass — no origin subtract, no
    /// divide, no Y-flip — and vertices come out in the exact units the
    /// primitive was authored in.
    ///
    /// This is what a caller wants when something downstream owns the
    /// projection: `GESpace` composes a per-object model→NDC matrix, and
    /// feeding it geometry that TE already baked to NDC would project it twice.
    /// It is also the only way to get an undistorted 3D primitive — baking to
    /// NDC divides X by the viewport width and Y by its height, so a sphere
    /// authored round comes out elliptical under a non-square viewport.
    ///
    /// Local space wins over any viewport: when this is true, `viewport` (and
    /// the call argument, and the effective viewport) are all ignored.
    bool localSpace = false;

    /// @brief The front-face winding this geometry is authored for.
    ///
    /// Like `viewport`, the winding a mesh wants is a property of the geometry
    /// rather than of the call that triangulates it, so it belongs here. When
    /// set, it **wins over** the `frontFaceRotation` argument on
    /// `triangulateSync` / `triangulateAsync` / `triangulateOnGPU`. Full
    /// precedence: `params.frontFaceRotation` → call argument →
    /// `GTEPolygonFrontFaceRotation::Clockwise`.
    ///
    /// Left unset by default so the existing call argument keeps working
    /// untouched for every caller that predates this field.
    std::optional<GTEPolygonFrontFaceRotation> frontFaceRotation;

    ~TETriangulationParams();
};

/**
 Holds the single mesh that results from a triangulation operation.
 Every triangulation path produces exactly one TEMesh.
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
        } topology = TopologyTriangle;
        /// One triangle corner: a position plus optional per-vertex
        /// attachment data (color / UVs / normal). Named (rather than
        /// anonymous, as `Polygon` used to declare it) so it can also be
        /// used standalone as `IndexedData::vertices`' element type.
        struct OMEGAGTE_EXPORT Vertex {
            GPoint3D pt;
            std::optional<AttachmentData> attachment;
        };
        struct OMEGAGTE_EXPORT Polygon {
            Vertex a,b,c;
        };
        std::vector<Polygon> vertexPolygons;

        /// Deduplicated vertex/index representation of `vertexPolygons`,
        /// populated by `buildIndexed()`. Empty until that method runs.
        struct OMEGAGTE_EXPORT IndexedData {
            std::vector<Vertex> vertices;
            std::vector<uint32_t> indices;
        };
        std::optional<IndexedData> indexedData;

        /// Deduplicate `vertexPolygons` into `indexedData`. Two corners
        /// collapse into one vertex only when their positions match within
        /// `positionEpsilon` AND their attachment data (color, UVs, normal)
        /// is equal — corners that share a position but differ in normal or
        /// UV (hard edges, UV seams) are intentionally kept distinct.
        /// @param positionEpsilon Position quantization grid size.
        void buildIndexed(float positionEpsilon = 1e-6f);

        // These CPU transforms mutate already-NDC-baked vertices: translate only
        // rescales its delta into NDC, and rotate spins vertices in NDC space, so
        // a rotation distorts under a non-square viewport. Superseded by GESpace,
        // which composes a per-object model->NDC matrix in true space units.
        OMEGA_DEPRECATED("Place the mesh in a GESpace and use its matrix transforms; TEMesh CPU transforms bake into NDC and distort under non-square viewports.")
        void translate(float x,float y,float z,const GEViewport & viewport);
        OMEGA_DEPRECATED("Place the mesh in a GESpace and use its matrix transforms; TEMesh::rotate spins NDC-space vertices and distorts under non-square viewports.")
        void rotate(float pitch,float yaw,float roll);
        OMEGA_DEPRECATED("Place the mesh in a GESpace and use its matrix transforms; TEMesh CPU transforms bake into NDC.")
        void scale(float w,float h,float l);
        unsigned vertexCount();
    };
    TEMesh mesh;
    unsigned totalVertexCount();
    OMEGA_DEPRECATED("Place the result's mesh in a GESpace and use its matrix transforms; these CPU transforms bake into NDC and distort under non-square viewports.")
    void translate(float x,float y,float z,const GEViewport & viewport);
    OMEGA_DEPRECATED("Place the result's mesh in a GESpace and use its matrix transforms; rotate spins NDC-space vertices and distorts under non-square viewports.")
    void rotate(float pitch,float yaw,float roll);
    OMEGA_DEPRECATED("Place the result's mesh in a GESpace and use its matrix transforms; these CPU transforms bake into NDC.")
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

    /// @brief Resolve which viewport a triangulation request is authored against.
    ///
    /// Single authority for the precedence rule, shared by the CPU path
    /// (`_triangulatePriv`) and every backend's `triangulateOnGPU`, so the two
    /// cannot drift apart:
    ///
    ///   `params.viewport` → `viewportArg` → `getEffectiveViewport()`
    ///
    /// A resolved viewport with zero width or height has no usable mapping; it
    /// is reported and replaced with the effective viewport rather than being
    /// allowed to divide by zero. Callers honoring `params.localSpace` skip the
    /// mapping entirely, so the returned viewport is unused in that case.
    GEViewport resolveViewport(const TETriangulationParams & params, GEViewport * viewportArg);

    /// @brief Resolve which front-face winding a triangulation request wants.
    ///
    /// The winding counterpart of `resolveViewport`, with the same precedence
    /// rule and the same reason for existing — the CPU path and every backend's
    /// GPU dispatch must agree on it:
    ///
    ///   `params.frontFaceRotation` → `frontFaceRotationArg` → `Clockwise`
    GTEPolygonFrontFaceRotation resolveWinding(const TETriangulationParams & params, GTEPolygonFrontFaceRotation frontFaceRotationArg);
    // Default Value: 0.01 radians.
    void setArcStep(float newArcStep){
        arcStep = newArcStep;
    };
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
    virtual ~OmegaTriangulationEngineContext();
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
        Create a Triangulation Context from a GENativeRenderTarget. Internally
        the context binds to the render target's `presentQueue()`.
        @param[in] renderTarget
        @returns SharedHandle<OmegaTriangulationEngineContext>
    */
    SharedHandle<OmegaTriangulationEngineContext> createTEContextFromNativeRenderTarget(SharedHandle<GENativeRenderTarget> & renderTarget);

    /**
        Create a Triangulation Context from a GETextureRenderTarget.
        Because texture render targets are queue-free, the caller must supply
        the command queue the tessellation work should be submitted on.
        @param[in] renderTarget
        @param[in] queue The command queue tessellation work runs on.
        @returns SharedHandle<OmegaTriangulationEngineContext>
    */
    SharedHandle<OmegaTriangulationEngineContext> createTEContextFromTextureRenderTarget(SharedHandle<GETextureRenderTarget> & renderTarget,
                                                                                          SharedHandle<GECommandQueue> & queue);
};

_NAMESPACE_END_

#endif
