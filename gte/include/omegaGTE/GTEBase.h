
#include <cmath>
#include <cassert>
#include <string>
#include <sstream>
#include <algorithm>
#include <iostream>

#include <omega-common/utils.h>
#include <omega-common/format.h>


#ifndef OMEGAGTE_GTEBASE_H
#define OMEGAGTE_GTEBASE_H

#ifdef TARGET_DIRECTX
#ifdef OMEGAGTE__BUILD__
#define OMEGAGTE_EXPORT __declspec( dllexport )
#else
#define OMEGAGTE_EXPORT __declspec( dllimport )
#endif
#else
#define OMEGAGTE_EXPORT
#endif

#define IN_INIT_SCOPE friend GTE Init();

#define _NAMESPACE_BEGIN_ namespace OmegaGTE {
#define _NAMESPACE_END_ }


_NAMESPACE_BEGIN_
    using namespace OmegaCommon;
    typedef enum : int {
        CodeOk,
        CodeFailed
    } StatusCode;

    /// A vector that preallocates a certain amount of memory but can be resized at any time.
    template<class T>
    class VectorHeap {
        T * _data;
    public:
        using size_type = unsigned;
        using iterator = T *;
        using const_iterator = const T*;
        using reference = T &;
        using const_reference = const T &;
    private:
        size_type maxSize;
        size_type len;
        void _push_el(const T & el){
            assert(len < maxSize && "Maximum size of VectorHeap has been hit, please resize.");
            auto dest = _data + len;
            memmove(_data + len,&el,sizeof(T));
            ++len;
        };
    public:
        iterator begin(){ return iterator(_data);};
        iterator end(){ return iterator(_data + len);};
        const_iterator cbegin() const{ return const_iterator(_data);};
        const_iterator cend() const{ return const_iterator(_data + len);};

        reference first(){ return begin()[0];};
        reference last(){ return end()[-1];};

        bool empty(){return len == 0;};

        bool full(){return len == maxSize;}

        void resize(unsigned newSize){
            auto temp = ::new T[newSize];
            std::move(begin(),end(),temp);
            delete [] _data;
            maxSize = newSize;
            _data = temp;
        };

        void push(const T & el){ _push_el(el);};
        void push(T && el){ _push_el(el);};
        void pop(){
            assert(len > 0 && "Cannot call pop() when VectorHeap is empty.");
            auto _end = last();
            _end.~T();
            --len;
        };
        size_type capacity(){
            return maxSize;
        };
        size_type length(){ return len; };

        explicit VectorHeap(unsigned maxSize):_data(::new T[maxSize]),maxSize(maxSize),len(0){

        };
        VectorHeap(const VectorHeap & other){
            maxSize = other.maxSize;
            len = other.len;
            _data = ::new T[maxSize];
            memcpy(_data,other.cbegin(),other.len * sizeof(T));
        };

        VectorHeap(VectorHeap & other){
            maxSize = other.maxSize;
            len = other.len;
            _data = ::new T[maxSize];
            memcpy(_data,other.begin(),other.len * sizeof(T));
        };
        // VectorHeap operator=(VectorHeap &other){
        //     maxSize = other.maxSize;
        //     len = other.len;
        //     _data = alloc.allocate(maxSize);
        //     std::copy(other.begin(),other.end(),begin());
        // };
        VectorHeap(VectorHeap && other){
            maxSize = other.maxSize;
            len = other.len;
            _data = ::new T[maxSize];
            memcpy(_data,other._data,other.len * sizeof(T));
        };
        ~VectorHeap(){
            if(_data != nullptr){
                for(auto & obj : *this){
                    obj.~T();
                };
                delete [] _data;
            }
        };
    };


    struct  OMEGAGTE_EXPORT GPoint2D {
        float x,y;
    };

    struct  OMEGAGTE_EXPORT GArc {
        GPoint2D center;
        float radians;
        unsigned radius_x;
        unsigned radius_y;
    };
    struct  OMEGAGTE_EXPORT GPoint3D {
        float x,y,z = 0;
    };

    struct  OMEGAGTE_EXPORT GRect {
        GPoint2D pos;
        float w,h;
    };

    struct  OMEGAGTE_EXPORT GRoundedRect {
        GPoint2D pos;
        float w,h,rad_x,rad_y;
    };

    struct  OMEGAGTE_EXPORT GRectangularPrism {
        GPoint3D pos;
        float w,h,d;
    };

    struct  OMEGAGTE_EXPORT GCylinder {
        GPoint3D pos;
        float r,h;
    };

    struct  OMEGAGTE_EXPORT GPyramid {
        float x,y,z,w,d,h;
    };

    struct  OMEGAGTE_EXPORT GCone {
        float x,y,z,r,h;
    };

    struct  OMEGAGTE_EXPORT GEllipsoid {
        float x,y,z,rad_x,rad_y,rad_z;
    };

    struct  OMEGAGTE_EXPORT GTorus {
        GPoint3D center;
        float majorRadius;  // R — center of torus to center of tube
        float minorRadius;  // r — tube radius
    };

    struct  OMEGAGTE_EXPORT GSphere {
        GPoint3D center;
        float radius;
    };

    struct  OMEGAGTE_EXPORT GCapsule {
        GPoint3D pos;    // center of the bottom hemisphere
        float radius;
        float height;    // distance between hemisphere centers (total height = height + 2*radius)
    };


    template<class Pt_Ty>
    class  GVectorPath_Base {
        struct Magnitude {
            static float calc(Pt_Ty & a,Pt_Ty & b);
        };
        public:
        struct Node {
            Pt_Ty *pt;
            Node *next = nullptr;
            Node(Pt_Ty *pt):pt(pt){};
            ~Node(){
                delete pt;
                if(next){
                    delete next;
                };
            };
        };

        struct  Segment {
            Pt_Ty * pt_A;
            Pt_Ty * pt_B;
        };

        class  Path_Iterator {
            Node *pt_A;
            Node *pt_B;
            unsigned pos;
            OMEGA_NODISCARD bool atEnd() const {
                return pt_A == nullptr || pt_B == nullptr;
            }
        public:
            explicit Path_Iterator(Node *_data):pt_A(_data),pt_B(nullptr),pos(0){
                if(pt_A != nullptr){
                    pt_B = pt_A->next;
                }
            };
            Path_Iterator & operator++(){
                if(pt_A == nullptr){
                    ++pos;
                    return *this;
                }

                if(pt_B == nullptr){
                    pt_A = nullptr;
                    ++pos;
                    return *this;
                }

                pt_A = pt_A->next;
                pt_B = (pt_A != nullptr) ? pt_A->next : nullptr;
                ++pos;
                return *this;
            };
            bool operator==(const Path_Iterator & r){
                if(atEnd() && r.atEnd()){
                    return true;
                }
                return pos == r.pos;
            };
            bool operator!=(const Path_Iterator & r){
                return !operator==(r);
            };
            Path_Iterator operator+(unsigned num){
                while(num > 0){
                    operator++();
                    --num;
                };
                return *this;
            };
            Segment operator*(){
                if(pt_A == nullptr || pt_B == nullptr){
                    return {nullptr,nullptr};
                }
                return {(pt_A->pt),(pt_B->pt)};
            };

        };
        using size_ty = unsigned;
        private:
        Node *first;
        float numPoints = 0;
        size_ty len = 0;
        public:
        using iterator = Path_Iterator;
        iterator begin(){
            return iterator(first);
        };
        iterator end(){
            return iterator(first) + (len);
        };
        const size_ty & size(){ return len;};
        private:
        void _push_pt(const Pt_Ty & pt){
            Node ** pt_b = &first->next;
            unsigned idx = len;
            while(idx > 0){
                pt_b = &((*pt_b)->next);
                --idx;
            };
            *(pt_b) = new Node(new Pt_Ty(std::move(pt)));
            ++len;
        };
        void assign(const GVectorPath_Base & other){
            assert(other.first != nullptr && "Cannot copy from an empty vector path");
            first = new Node(new Pt_Ty(*(other.first->pt)));

            Node *dst = first;
            Node *src = other.first->next;
            while(src != nullptr){
                dst->next = new Node(new Pt_Ty(*(src->pt)));
                dst = dst->next;
                src = src->next;
            }

            len = other.len;
            numPoints = other.numPoints;
        }
        public:
        Pt_Ty & firstPt(){
            return *(first->pt);
        };
        Pt_Ty & lastPt(){
            if(len > 0) {
                auto it = begin();
                auto prev_it = it;
                while (it != end()) {
                    prev_it = it;
                    ++it;
                }
                return *(*prev_it).pt_B;
            }
            else {
                return firstPt();
            }
        };
        void append(const Pt_Ty &pt){
            return _push_pt(pt);
        };
        void append(Pt_Ty &&pt){
            return _push_pt(pt);
        };
        std::string toStr(){
            std::ostringstream out_;
            out_ << "VectorPath Size:" << size() << std::endl;
            auto it = begin();

            while(it != end()){
                auto segment = *it;
                Pt_Ty *pt_A = *segment.pt_A;
                Pt_Ty *pt_B = *segment.pt_B;
                if(sizeof(Pt_Ty) == sizeof(GPoint2D)){
                    out_ << "Segment {" << "[x:" << pt_A->x << ",y:" << pt_A->y << "] [x:" << pt_B->x << ",y:" << pt_B->y << "] }"<< std::endl;
                }
                else if(sizeof(Pt_Ty) == sizeof(GPoint3D)){
//                        out_ << "Segment {" << "[x:" << pt_A->x << ",y:" << pt_A->y << ",z:" << pt_A->z << "] [x:" << pt_B->x << ",y:" << pt_B->y << ",z:" << pt_B->z << "] }"<< std::endl;
                };
                ++it;
            };
            return out_.str();
        };
        float mag(){
            float sum = 0;
            for(auto it = begin();it != end();it.operator++()){
                auto seg = *it;
                sum += Magnitude::calc(*seg.pt_A,*seg.pt_B);
            }
            return sum;
        }
        /// Apply a mutating function to every point of the path, in order.
        /// Unlike the segment iterator, this visits each underlying point
        /// exactly once (including the start point and the final point).
        template<class Fn>
        void transformEachPoint(Fn && fn){
            for(Node *n = first; n != nullptr; n = n->next){
                fn(*(n->pt));
            }
        }
        void reset(const Pt_Ty & start){
            delete first;
            len = 0;
            numPoints = 1;
            first = new Node(new Pt_Ty(std::move(start)));
        };
        GVectorPath_Base(const Pt_Ty & start):first(new Node(new Pt_Ty(std::move(start)))),len(0),numPoints(1){};
        GVectorPath_Base(Pt_Ty &&start):first(new Node(new Pt_Ty(std::move(start)))),len(0),numPoints(1){};
        GVectorPath_Base() = delete;
        GVectorPath_Base(const GVectorPath_Base<Pt_Ty> &other){
            assign(other);
        };
        GVectorPath_Base(GVectorPath_Base<Pt_Ty> && other) noexcept{
            assign(other);
        };
        GVectorPath_Base & operator=(const GVectorPath_Base<Pt_Ty> &other){
            if(this == &other){
                return *this;
            }
            delete first;
            first = nullptr;
            assign(other);
            return *this;
        }
        GVectorPath_Base & operator=(GVectorPath_Base<Pt_Ty> &&other) noexcept {
            if(this == &other){
                return *this;
            }
            delete first;
            first = nullptr;
            assign(other);
            return *this;
        }
        ~GVectorPath_Base(){
            delete first;
        };
    };

    template<>
    struct GVectorPath_Base<GPoint2D>::Magnitude {
        static float calc(GPoint2D & a,GPoint2D & b){
            auto x = (b.x - a.x);
            auto y = (b.y - a.y);
            return std::sqrt((x * x) + (y * y));
        };
    };

    typedef GVectorPath_Base<GPoint2D> GVectorPath2D;

    template<>
    struct GVectorPath_Base<GPoint3D>::Magnitude {
        static float calc(GPoint3D & a,GPoint3D & b){
            auto x = (b.x - a.x);
            auto y = (b.y - a.y);
            auto z = (b.z - a.z);
            return std::sqrt((x * x) + (y * y) + (z * z));
        };
    };

    typedef GVectorPath_Base<GPoint3D> GVectorPath3D;

    // ====================================================================
    // GVectorPath transforms
    //
    // In-place affine transforms over every point of a path. Rotation
    // angles are in radians. The 3D Euler convention matches
    // OmegaGTE::rotationEuler (GTEMath.h): the composed rotation is
    // Rz(roll) * Ry(yaw) * Rx(pitch). Implemented with direct trig rather
    // than FMatrix because GTEMath.h depends on this header.
    // ====================================================================

    /// Translate every point of a 2D path by (dx, dy).
    inline void translate(GVectorPath2D & path, float dx, float dy){
        path.transformEachPoint([=](GPoint2D & p){
            p.x += dx;
            p.y += dy;
        });
    }

    /// Translate every point of a 3D path by (dx, dy, dz).
    inline void translate(GVectorPath3D & path, float dx, float dy, float dz){
        path.transformEachPoint([=](GPoint3D & p){
            p.x += dx;
            p.y += dy;
            p.z += dz;
        });
    }

    /// Rotate every point of a 2D path by `radians` about `pivot` (defaults
    /// to the origin). Uses the same handedness as the engine's Z-axis
    /// rotation (rotationZ / a `roll` in the 3D overload), so a 2D path and
    /// the same path embedded in the XY-plane and rotated by `roll = radians`
    /// agree.
    inline void rotate(GVectorPath2D & path, float radians, GPoint2D pivot = {0.f,0.f}){
        float c = std::cos(radians), s = std::sin(radians);
        path.transformEachPoint([=](GPoint2D & p){
            float x = p.x - pivot.x;
            float y = p.y - pivot.y;
            p.x = pivot.x + (c * x - s * y);
            p.y = pivot.y + (s * x + c * y);
        });
    }

    /// Rotate every point of a 3D path by the Euler angles `pitch` (about X),
    /// `yaw` (about Y) and `roll` (about Z) about `pivot` (defaults to the
    /// origin). Pitch is applied first, then yaw, then roll — the composed
    /// rotation is Rz(roll) * Ry(yaw) * Rx(pitch), matching
    /// OmegaGTE::rotationEuler and FQuaternion::fromEuler.
    inline void rotate(GVectorPath3D & path, float pitch, float yaw, float roll, GPoint3D pivot = {0.f,0.f,0.f}){
        float cx = std::cos(pitch), sx = std::sin(pitch);
        float cy = std::cos(yaw),   sy = std::sin(yaw);
        float cz = std::cos(roll),  sz = std::sin(roll);
        path.transformEachPoint([=](GPoint3D & p){
            float x = p.x - pivot.x;
            float y = p.y - pivot.y;
            float z = p.z - pivot.z;
            // Rx(pitch): x unchanged
            float y1 = cx * y - sx * z;
            float z1 = sx * y + cx * z;
            // Ry(yaw): y unchanged (y1)
            float x2 =  cy * x + sy * z1;
            float z2 = -sy * x + cy * z1;
            // Rz(roll): z unchanged (z2)
            float x3 = cz * x2 - sz * y1;
            float y3 = sz * x2 + cz * y1;
            p.x = pivot.x + x3;
            p.y = pivot.y + y3;
            p.z = pivot.z + z2;
        });
    }


    enum class GTEPolygonFrontFaceRotation : int {
        Clockwise,
        CounterClockwise
    };

    /// @brief Describes a pixel format for render targets, textures, and pipelines.
    /// @paragraph Values are grouped into numeric ranges (color = 0–63,
    /// depth/stencil = 64–95, BC = 96–127, ASTC = 128–159, ETC = 160–191) so a new
    /// entry can be appended within its group without renumbering its siblings —
    /// which would otherwise be a soft-ABI break for anything that has serialized a
    /// `PixelFormat`. Naming is `<channels><bits><type>[_SRGB]`; compressed families
    /// put the block size first (`ASTC_4x4_Unorm`). See PixelFormat-Completion-Plan.md.
    enum class PixelFormat : int {
        // ── 8-bit color ──────────────────────────────────────────────
        R8Unorm                     = 0,    // glyph atlas, single-channel masks
        R8Snorm                     = 1,
        R8Uint                      = 2,
        RG8Unorm                    = 3,
        RG8Snorm                    = 4,
        RGBA8Unorm                  = 5,
        RGBA8Unorm_SRGB             = 6,
        RGBA8Snorm                  = 7,
        BGRA8Unorm                  = 8,    // Windows swapchain default
        BGRA8Unorm_SRGB             = 9,

        // ── 16-bit color ─────────────────────────────────────────────
        R16Unorm                    = 16,
        R16Float                    = 17,
        R16Uint                     = 18,
        RG16Unorm                   = 19,
        RG16Float                   = 20,
        RGBA16Unorm                 = 21,
        RGBA16Float                 = 22,   // HDR framebuffer / G-buffer normals

        // ── 32-bit color ─────────────────────────────────────────────
        R32Float                    = 32,
        R32Uint                     = 33,
        RG32Float                   = 34,
        RGBA32Float                 = 35,   // ground-truth HDR / G-buffer world pos

        // ── Packed ───────────────────────────────────────────────────
        RGB10A2Unorm                = 48,
        R11G11B10Float              = 49,

        // ── Depth / stencil ──────────────────────────────────────────
        D16Unorm                    = 64,
        D32Float                    = 65,
        D24Unorm_S8Uint             = 66,
        D32Float_S8Uint             = 67,

        // ── Block-compressed: BC (desktop; NOT on Apple Silicon) ─────
        BC1_RGBA_Unorm              = 96,
        BC1_RGBA_Unorm_SRGB         = 97,
        BC3_RGBA_Unorm              = 98,
        BC3_RGBA_Unorm_SRGB         = 99,
        BC5_RG_Unorm                = 100,
        BC7_RGBA_Unorm              = 101,
        BC7_RGBA_Unorm_SRGB         = 102,

        // ── Block-compressed: ASTC (mobile / Apple) ──────────────────
        ASTC_4x4_Unorm              = 128,
        ASTC_4x4_Unorm_SRGB         = 129,
        ASTC_6x6_Unorm              = 130,
        ASTC_6x6_Unorm_SRGB         = 131,
        ASTC_8x8_Unorm              = 132,
        ASTC_8x8_Unorm_SRGB         = 133,

        // ── Block-compressed: ETC2 (Android baseline) ────────────────
        ETC2_RGB8_Unorm             = 160,
        ETC2_RGB8_Unorm_SRGB        = 161,
        ETC2_RGBA8_Unorm            = 162,
        ETC2_RGBA8_Unorm_SRGB       = 163,
        EAC_R11_Unorm               = 164
    };

    /// @brief Device-independent structural facts about a `PixelFormat` — bit
    /// counts, channel layout, block shape, and which attachment aspect it can
    /// serve. One source of truth, so the backends stop re-deriving
    /// `bytesPerTexel` from their own `switch` (which does not scale past a
    /// handful of formats).
    ///
    /// Device-DEPENDENT questions ("can *this* adapter render to this format?")
    /// are not answerable here — they belong on the engine.
    struct OMEGAGTE_EXPORT PixelFormatInfo {
        enum class Aspect : std::uint8_t {
            Color,
            Depth,
            Stencil,
            DepthStencil
        };

        Aspect        aspect        = Aspect::Color;
        /// Bytes per texel for an uncompressed format; 0 when compressed (use
        /// `blockBytes` and the block dimensions instead).
        std::uint8_t  bytesPerTexel = 4;
        std::uint8_t  blockWidth    = 1;   // 1 when uncompressed
        std::uint8_t  blockHeight   = 1;
        /// Bytes per compressed block; 0 when uncompressed.
        std::uint8_t  blockBytes    = 0;
        bool          isCompressed  = false;
        bool          isSRGB        = false;
        std::uint8_t  channelCount  = 4;

        /// True when the format can back a depth and/or stencil attachment.
        OMEGA_NODISCARD bool isDepthStencil() const {
            return aspect != Aspect::Color;
        }
    };

    /// @brief Structural facts about `fmt`. Total function — an unrecognized
    /// value yields the RGBA8Unorm-shaped default rather than throwing.
    OMEGAGTE_EXPORT PixelFormatInfo pixelFormatInfo(PixelFormat fmt);

    struct OMEGAGTE_EXPORT TextureRegion {
        unsigned x,y,z;
        unsigned w,h,d;
        /// Mip pyramid level this region addresses. Defaults to 0 so existing
        /// six-field aggregate initializers keep targeting the base level.
        /// (Pipeline-Completion-Extension-Plan §7.1.)
        unsigned mipLevel = 0;
        /// Array slice / cube-face index this region addresses. Defaults to 0.
        unsigned arrayLayer = 0;
    };

    /// @brief Individual channel source for a texture swizzle mapping.
    enum class TextureSwizzleChannel : unsigned char {
        Red,      ///< Source the red channel
        Green,    ///< Source the green channel
        Blue,     ///< Source the blue channel
        Alpha,    ///< Source the alpha channel
        Zero,     ///< Constant 0
        One,      ///< Constant 1
        Identity  ///< Passthrough (use the channel's own position)
    };

    /// @brief Describes how texture channels are routed when sampled or read.
    struct OMEGAGTE_EXPORT TextureSwizzle {
        TextureSwizzleChannel r = TextureSwizzleChannel::Identity;
        TextureSwizzleChannel g = TextureSwizzleChannel::Identity;
        TextureSwizzleChannel b = TextureSwizzleChannel::Identity;
        TextureSwizzleChannel a = TextureSwizzleChannel::Identity;

        /// Convenience: identity swizzle (no remapping).
        static TextureSwizzle identity() { return {}; }

        /// Convenience: broadcast red into all four channels.
        static TextureSwizzle broadcastRed() {
            return { TextureSwizzleChannel::Red,
                     TextureSwizzleChannel::Red,
                     TextureSwizzleChannel::Red,
                     TextureSwizzleChannel::Red };
        }

        /// Convenience: swap R and B channels (RGBA <-> BGRA).
        static TextureSwizzle swapRB() {
            return { TextureSwizzleChannel::Blue,
                     TextureSwizzleChannel::Green,
                     TextureSwizzleChannel::Red,
                     TextureSwizzleChannel::Alpha };
        }

        bool isIdentity() const {
            return r == TextureSwizzleChannel::Identity
                && g == TextureSwizzleChannel::Identity
                && b == TextureSwizzleChannel::Identity
                && a == TextureSwizzleChannel::Identity;
        }

        bool operator==(const TextureSwizzle & other) const {
            return r == other.r && g == other.g && b == other.b && a == other.a;
        }
        bool operator!=(const TextureSwizzle & other) const {
            return !operator==(other);
        }
    };

    /// @brief Memory allocated on a GTEDevice.
    struct OMEGAGTE_EXPORT GTEResource {
        /// @brief Set the name of the Resource
        virtual void setName(OmegaCommon::StrRef name) = 0;
        /// @brief Retrieves the underlying native type of this resource.
        virtual void *native() = 0;
    };

_NAMESPACE_END_

namespace OmegaCommon {
    template<>
    struct FormatProvider<OmegaGTE::GVectorPath2D> {
        static void format(std::ostream & out,OmegaGTE::GVectorPath2D & object){
            out << "OmegaGTE.GVectorPath2D";
        }
    };

    template<>
    struct FormatProvider<OmegaGTE::GVectorPath3D> {
        static void format(std::ostream & out,OmegaGTE::GVectorPath3D & object){
            out << "OmegaGTE.GVectorPath3D";
        }
    };
}

#endif
