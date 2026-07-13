
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
    enum class PixelFormat : int {
        RGBA8Unorm,
        RGBA16Unorm,
        RGBA8Unorm_SRGB,
        BGRA8Unorm,
        BGRA8Unorm_SRGB
    };

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
