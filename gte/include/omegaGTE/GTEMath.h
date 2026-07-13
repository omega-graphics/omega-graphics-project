#ifndef OMEGAGTE_GTEMATH_H
#define OMEGAGTE_GTEMATH_H

#include "GTEBase.h"
#include <cmath>
#include <array>

_NAMESPACE_BEGIN_

    OMEGAGTE_EXPORT extern const long double PI;


    template<class Num_Ty,typename Angle_Ty = float>
    class  Vector2D_Base {
        Num_Ty i;
        Num_Ty j;
    public:
        Vector2D_Base(Num_Ty _i,Num_Ty _j):i(_i),j(_j){

        };
        static Vector2D_Base FromMagnitudeAndAngle(Num_Ty mag,Angle_Ty angle){
            Vector2D_Base v(cos(angle) * mag,sin(angle) * mag);
            return v;
        };
        Num_Ty & getI(){
            return i;
        };
        Num_Ty & getJ(){
            return j;
        };
        /// Get magnitude
        virtual Num_Ty mag(){
            return sqrt(pow(i,2) + pow(j,2));
        };
        /// Get angle relative to `i` !
        Angle_Ty angle(){
            return atan(j/i);
        };
        protected:
        void add_to_s(const Vector2D_Base<Num_Ty,Angle_Ty> & vector2d){
            i += vector2d.i;
            j += vector2d.j;
        };
        void subtract_to_s(const Vector2D_Base<Num_Ty,Angle_Ty> & vector2d){
            i -= vector2d.i;
            j -= vector2d.j;
        };

        Vector2D_Base<Num_Ty,Angle_Ty> add(const Vector2D_Base<Num_Ty,Angle_Ty> & vector2d){
            auto _i = i + vector2d.i;
            auto _j = j + vector2d.j;
            return Vector2D_Base(_i,_j);
        };

        Vector2D_Base<Num_Ty,Angle_Ty> subtract(const Vector2D_Base<Num_Ty,Angle_Ty> & vector2d){
            auto _i = i - vector2d.i;
            auto _j = j - vector2d.j;
            return Vector2D_Base(_i,_j);
        };
        public:
        virtual Vector2D_Base<Num_Ty,Angle_Ty> operator+(const Vector2D_Base<Num_Ty,Angle_Ty> & vec){
            return add(std::move(vec));
        };
        virtual Vector2D_Base<Num_Ty,Angle_Ty> operator-(const Vector2D_Base<Num_Ty,Angle_Ty> & vec){
            return subtract(std::move(vec));
        };
        void operator+=(const Vector2D_Base<Num_Ty,Angle_Ty> & vec){
            add_to_s(std::move(vec));
        };
        void operator-=(const Vector2D_Base<Num_Ty,Angle_Ty> & vec){
            subtract_to_s(std::move(vec));
        };
        Num_Ty dot(const Vector2D_Base<Num_Ty,Angle_Ty> & vec){
            return (i * vec.i) + (j * vec.j);
        };
    };

    typedef Vector2D_Base<float,float> FVector2D;
    typedef Vector2D_Base<int,float> IVector2D;


    template<class Num_Ty,typename Angle_Ty = float>
    class  Vector3D_Base : protected Vector2D_Base<Num_Ty,Angle_Ty> {
        typedef Vector2D_Base<Num_Ty,Angle_Ty> parent;
        Num_Ty k;
        public:
        Vector3D_Base(Num_Ty _i,Num_Ty _j,Num_Ty _k):parent(_i,_j),k(_k){};
        Vector3D_Base(const parent & vec):parent(vec),k(0){

        }
        static Vector3D_Base FromMagnitudeAndAngles(Num_Ty mag,Angle_Ty angle_v,Angle_Ty angle_h){
            Vector3D_Base v(parent::FromMagnitudeAndAngle(mag,angle_v));
            v.k = sin(angle_h) * mag;
            return v;
        };

        using parent::getI;

        using parent::getJ;

        Num_Ty & getK(){
            return k;
        };
        /// Get magnitude
        virtual Num_Ty mag() override{
            return std::sqrt((this->getI() * this->getI()) + (this->getJ() * this->getJ()) + (k * k));
        }
        /// Get the angle on the horizontal plane (Measured from `i`);
        Angle_Ty angle_h(){
            return parent::angle();
        }
        /// Get the angle on the vertical plane (Measured from `i + k`)
        Angle_Ty angle_v(){
            return atan(this->getJ()/sqrt(this->getI() * this->getI() + (k * k)));
        };
        protected:

        void add_to_s(const Vector3D_Base<Num_Ty,Angle_Ty> & vector3d){
            parent::add_to_s(vector3d);
            k += vector3d.k;
        };

        void subtract_to_s(const Vector3D_Base<Num_Ty,Angle_Ty> & vector3d){
            parent::subtract_to_s(vector3d);
            k -= vector3d.k;
        };

        void add_to_s_vec2(const Vector2D_Base<Num_Ty,Angle_Ty> & vector2d){
            parent::subtract_to_s(vector2d);
        };

        void sub_to_s_vec2(const Vector2D_Base<Num_Ty,Angle_Ty> & vector2d){
            parent::subtract_to_s(vector2d);
        };

        Vector3D_Base<Num_Ty,Angle_Ty> add(const Vector3D_Base<Num_Ty,Angle_Ty> & vector3d){
            Vector3D_Base v(this->getI(),this->getJ(),k);
            v.add_to_s(vector3d);
            return v;
        };

        Vector3D_Base<Num_Ty,Angle_Ty> subtract(const Vector3D_Base<Num_Ty,Angle_Ty> & vector3d){
            Vector3D_Base v(this->getI(),this->getJ(),k);
            v.subtract_to_s(vector3d);
            return v;
        };


        public:

        /// Operators
        /// @{
//        Vector3D_Base<Num_Ty,Angle_Ty> operator+(const Vector2D_Base<Num_Ty,Angle_Ty> & vec){
//            return add(std::move(vec));
//        };

        Vector3D_Base<Num_Ty,Angle_Ty> operator+(const Vector3D_Base<Num_Ty,Angle_Ty> & vec){
            return add(std::move(vec));
        };

//        Vector3D_Base<Num_Ty,Angle_Ty> operator-(const Vector2D_Base<Num_Ty,Angle_Ty> & vec){
//            return subtract(std::move(vec));
//        };

        Vector3D_Base<Num_Ty,Angle_Ty> operator-(const Vector3D_Base<Num_Ty,Angle_Ty> & vec){
            return subtract(std::move(vec));
        };


        void operator+=(const Vector3D_Base<Num_Ty,Angle_Ty> & vec){
            add_to_s(std::move(vec));
        };;

        void operator-=(const Vector3D_Base<Num_Ty,Angle_Ty> & vec){
            subtract_to_s(std::move(vec));
        };
        /// @}

        /// Vector Transformations
        /// @{
        Num_Ty dot(const Vector3D_Base<Num_Ty,Angle_Ty> & vec){
            return parent::dot(vec) + (k * vec.k);
        };

        Vector3D_Base<Num_Ty,Angle_Ty> cross(Vector3D_Base<Num_Ty,Angle_Ty> & vec){
            Num_Ty i_res = ((this->getI() * vec.k) - (k * vec.getI()));
            Num_Ty j_res = -((this->getI() * vec.k) - (k * vec.getI()));
            Num_Ty k_res = ((this->getI() * vec.getJ()) - (this->getJ() * vec.getI()));
            return Vector3D_Base(i_res,j_res,k_res);
        };
        /// @}
    };

    typedef Vector3D_Base<int,float>  IVector3D;
    typedef Vector3D_Base<float,float> FVector3D;


    /// Generic dense matrix over arithmetic type `Ty`, sized at compile time.
    ///
    /// **Template parameter order: `(Ty, column, row)`.** The COLUMN COUNT
    /// comes BEFORE the row count — `Matrix<float, 3, 4>` has 3 columns and
    /// 4 rows, not the other way around. The public aliases follow the same
    /// order: `FMatrix<c, r> = Matrix<float, c, r>` and
    /// `FVec<n> = FMatrix<n, 1> = Matrix<float, n, 1>` (n columns, 1 row).
    ///
    /// **Storage is column-major.** The underlying buffer is
    /// `std::array<std::array<Ty, row>, column>` — consecutive memory holds
    /// one whole column, then the next column. This matches the convention
    /// of OpenGL, Metal, GLSL, MSL, and HLSL `column_major` (the GTE shader
    /// codegen targets), so a matrix uploaded into a GPU buffer crosses the
    /// boundary without a transpose.
    ///
    /// **Indexing is `m[col][row]`** — the OUTER subscript is the COLUMN
    /// index, the inner is the row index. To translate from standard
    /// mathematical notation `M(row, col)` (row first), use:
    /// ```
    ///   M(row=j, col=i)  ==  m_GTE[col=i][row=j]  ==  m[i][j]
    /// ```
    /// So `m[i][j]` is the entry at row `j`, column `i`. Code that assumes
    /// row-major (the more familiar `m[row][col]` C convention) will silently
    /// produce the TRANSPOSE of the intended matrix — a class of bug that
    /// passes any eigenvalue-only test (eigenvalues are invariant under
    /// transpose) and only surfaces when the matrix is composed with a frame
    /// transform or fed to `Quaternion::fromMatrix`. See
    /// `AQdiagonalizeInertia` in `aqua/include/aqua/AQMath.h` and the
    /// research note in `aqua/.plans/Phase-1-Dynamics-Math-Core.md §12.2`
    /// for a documented case that exposed it.
    ///
    /// **Vectors are `Matrix<Ty, n, 1>`** — n columns × 1 row, with the i-th
    /// component at `v[i][0]`. This is how AQUA's `AQVec3 = Matrix<Ty, 3, 1>`
    /// is indexed throughout `aqua/include/aqua/AQMath.h`.
    ///
    /// **Construction.** The default constructor is PRIVATE; use the
    /// factory statics `Matrix::Create()` (zero-initialized) or
    /// `Matrix::Identity()` (only valid for square matrices). Component
    /// values are then assigned per-entry — there is no brace-init
    /// component constructor. Members of value types that embed Matrix
    /// or `FVec<n>` must therefore default-initialize them with `Create()`
    /// (the pattern `AQBodyState` and `AQDebugLine` use in AQUA).
    ///
    /// **Quaternion bridge.** `Quaternion::toMatrix()` and
    /// `Quaternion::fromMatrix()` honor this convention internally: the
    /// 4×4 they produce / consume has its standard-math entry at
    /// `(row=j, col=i)` stored in `m[i][j]`, so quaternion-rotation
    /// matrices line up with the conventional textbook formulas without
    /// an implicit transpose.
    template<class Ty,unsigned column,unsigned row>
   class Matrix {
   public:
       typedef typename std::array<Ty,row>::iterator row_pointer;
       typedef typename std::array<Ty,row>::const_iterator const_row_pointer;
       typedef typename std::array<std::array<Ty,row>,column>::iterator column_pointer;
       typedef unsigned size_type;

       class row_pointer_wrapper {
            row_pointer pt;
       public:
           explicit row_pointer_wrapper(row_pointer _pt):pt(_pt){}

//            row_pointer_wrapper(const row_pointer_wrapper &) = delete;

            typedef row_pointer iterator;
            typedef Ty & reference;

            inline iterator begin(){
                return pt;
            }
            inline iterator end(){
                return pt + row;
            }

            inline Ty & at(size_type idx){
                assert(idx < row && "Cannot index row value at index");
                return pt[idx];
            }
            inline Ty & at(size_type idx) const{
                assert(idx < row && "Cannot index row value at index");
                return pt[idx];
            }
            inline Ty & operator[](size_type idx){
                return at(idx);
            }
            inline Ty & operator[](size_type idx) const{
                return at(idx);
            }
       };

       class const_row_pointer_wrapper {
            const_row_pointer pt;
       public:
           explicit const_row_pointer_wrapper(const_row_pointer _pt):pt(_pt){}

            typedef const_row_pointer iterator;
            typedef const Ty & reference;

            inline iterator begin(){
                return pt;
            }
            inline iterator end(){
                return pt + row;
            }

            inline const Ty & at(size_type idx) const{
                assert(idx < row && "Cannot index row value at index");
                return pt[idx];
            }
            inline const Ty & operator[](size_type idx) const{
                return at(idx);
            }
       };



       class row_pointer_wrapper_iterator {
           column_pointer pt;
       public:
           explicit row_pointer_wrapper_iterator(column_pointer _pt):pt(_pt){

           }
           void operator +=(size_type n){
               pt += n;
           }
           void operator ++(){
               operator+=(1);
           }
           bool operator !=(const row_pointer_wrapper_iterator &other){
               return other.pt != pt;
           }
           row_pointer_wrapper operator *(){
               return {*pt};
           }

       };

       typedef row_pointer_wrapper_iterator iterator;
    private:
        std::array<std::array<Ty,row>,column> _data;

        inline void alloc_matrix_mem(Ty * dest){
//            dest = new row_pointer [column];
//            column_pointer _data_it = dest;
//            unsigned _c = column;
//            while(_c > 0){
//                *_data_it = 0;
//                ++_data_it;
//                --_c;
//            }
        }

        Matrix(){
            /// Alloc Matrix Mem
            alloc_matrix_mem(nullptr);

            /// Set all Matrix vals to 0

            for(unsigned i = 0;i < column;i++){
                for(unsigned j = 0;j < row;j++){
                    _data[i][j] = 0;
                }
            }
        };

        template<unsigned _column,unsigned _row>
        static void copy_data_to(column_pointer from,column_pointer dest){
            column_pointer _data_it = dest;
            column_pointer _data_it_f = from;
            unsigned _c = _column;
            while(_c > 0){
                row_pointer _row_it = _data_it->begin();
                row_pointer _row_it_f = _data_it_f->begin();
                unsigned _r = _row;
                while(_r > 0){
                    *_row_it = *_row_it_f;
                    --_r;
                    ++_row_it;
                    ++_row_it_f;
                }

                ++_data_it;
                ++_data_it_f;
                --_c;
            }
        }
   public:
       inline iterator begin(){
            return {_data.begin()};
        }
        inline iterator end(){
            return {_data.end()};
        }
       inline row_pointer_wrapper at(size_type idx){
           assert(idx < column && "Cannot index column pointer at index");
           return row_pointer_wrapper(_data[idx].begin());
       }
       inline const_row_pointer_wrapper at(size_type idx) const{
           assert(idx < column && "Cannot index column pointer at index");
           return const_row_pointer_wrapper(_data[idx].begin());
       }

       inline row_pointer_wrapper operator[](size_type idx){
            return at(idx);
       };

       inline const_row_pointer_wrapper operator[](size_type idx) const{
           return at(idx);
       };

       template<unsigned n_column,unsigned n_row>
       Matrix<Ty,n_column,n_row> resize(){
           Matrix<Ty,n_column,n_row> n;
           unsigned col_to_cpy,row_to_cpy;
           /// Downsizing Matrix Column Count
           if(n_column > column){
             col_to_cpy = column;
           }
           else {
               col_to_cpy = n_column;
           }
           /// Downsizing Matrix Row Count
           if(n_row > row){
               row_to_cpy = row;
           }
           else {
               row_to_cpy = n_row;
           }
           copy_data_to<col_to_cpy,row_to_cpy>(_data.begin(),n._data.begin());
           return n;
       }

       template<unsigned o_column,unsigned o_row>
        explicit Matrix(const Matrix<Ty,o_column,o_row> & other){
            alloc_matrix_mem(nullptr);
            unsigned col_to_cpy,row_to_cpy;
            /// Downsizing Matrix Column Count
            if(o_column > column){
                col_to_cpy = column;
            }
            else {
                col_to_cpy = o_column;
            }

            /// Downsizing Matrix Row Count
            if(o_row > row){
                row_to_cpy = row;
            }
            else {
                row_to_cpy = o_row;
            }
            copy_data_to<col_to_cpy,row_to_cpy>(other._data.begin(),_data.begin());
        };
    //    template<unsigned o_column,unsigned o_row>
    //    explicit Matrix(Matrix<Ty,o_column,o_row> && other){
    //         // alloc_matrix_mem(nullptr);
    //         // unsigned col_to_cpy,row_to_cpy;

    //         // copy_data_to<o_column,o_row>(other._data.begin(),_data.begin());
    //     };

        /// Construct a Matrix from a Vector2D
        explicit Matrix(Vector2D_Base<Ty> & vec){
            static_assert(row == 1 && column == 2 && "Cannot construct Matrix of size from Vector2D");
//            alloc_matrix_mem(_data.data());
            this->operator[](0).operator[](0) = vec.getI();
            this->operator[](1).operator[](0) = vec.getJ();
        }
        /// Construct a Matrix from a Vector3D
        explicit Matrix(Vector3D_Base<Ty> & vec){
            static_assert(row == 1 && column == 3 && "Cannot construct Matrix of size from Vector2D");
//            alloc_matrix_mem(_data.data());
            this->operator[](0).operator[](0) = vec.getI();
            this->operator[](1).operator[](0) = vec.getJ();
            this->operator[](2).operator[](0) = vec.getK();
        }

       /** @brief Create an empty Matrix with the specified width and height.
           @param[in] h Height
           @param[in] w Width
           @returns Matrix
       */
       static Matrix Create(){
           return Matrix();
       };
       static Matrix Identity(){
           static_assert(column == row && "Cannot construct an Identity Matrix");
           auto m = Create();

           for(unsigned _c = 0;_c < column;_c++){
                m[_c][_c] = 1;
           }
           return m;
       };
       ~Matrix() = default;

       // ---- Arithmetic operators ----

       template<class, unsigned, unsigned> friend class Matrix;

       Matrix operator+(const Matrix& other) const {
           auto r = Create();
           for(unsigned i = 0; i < column; i++)
               for(unsigned j = 0; j < row; j++)
                   r._data[i][j] = _data[i][j] + other._data[i][j];
           return r;
       }
       Matrix operator-(const Matrix& other) const {
           auto r = Create();
           for(unsigned i = 0; i < column; i++)
               for(unsigned j = 0; j < row; j++)
                   r._data[i][j] = _data[i][j] - other._data[i][j];
           return r;
       }
       Matrix operator-() const {
           auto r = Create();
           for(unsigned i = 0; i < column; i++)
               for(unsigned j = 0; j < row; j++)
                   r._data[i][j] = -_data[i][j];
           return r;
       }
       Matrix operator*(Ty scalar) const {
           auto r = Create();
           for(unsigned i = 0; i < column; i++)
               for(unsigned j = 0; j < row; j++)
                   r._data[i][j] = _data[i][j] * scalar;
           return r;
       }
       friend Matrix operator*(Ty scalar, const Matrix& m) { return m * scalar; }

       /// Matrix * Matrix: (CxR) * (RxP) = (CxP)
       template<unsigned P>
       Matrix<Ty, column, P> operator*(const Matrix<Ty, row, P>& other) const {
           auto r = Matrix<Ty, column, P>::Create();
           for(unsigned i = 0; i < column; i++)
               for(unsigned j = 0; j < P; j++){
                   Ty sum = 0;
                   for(unsigned k = 0; k < row; k++)
                       sum += _data[i][k] * other._data[k][j];
                   r._data[i][j] = sum;
               }
           return r;
       }

       Matrix& operator+=(const Matrix& other){
           for(unsigned i = 0; i < column; i++)
               for(unsigned j = 0; j < row; j++)
                   _data[i][j] += other._data[i][j];
           return *this;
       }
       Matrix& operator-=(const Matrix& other){
           for(unsigned i = 0; i < column; i++)
               for(unsigned j = 0; j < row; j++)
                   _data[i][j] -= other._data[i][j];
           return *this;
       }
       Matrix& operator*=(Ty scalar){
           for(unsigned i = 0; i < column; i++)
               for(unsigned j = 0; j < row; j++)
                   _data[i][j] *= scalar;
           return *this;
       }

       bool operator==(const Matrix& other) const {
           for(unsigned i = 0; i < column; i++)
               for(unsigned j = 0; j < row; j++)
                   if(_data[i][j] != other._data[i][j]) return false;
           return true;
       }
       bool operator!=(const Matrix& other) const { return !(*this == other); }

       // ---- Utility ----

       Matrix<Ty, row, column> transposed() const {
           auto r = Matrix<Ty, row, column>::Create();
           for(unsigned i = 0; i < column; i++)
               for(unsigned j = 0; j < row; j++)
                   r._data[j][i] = _data[i][j];
           return r;
       }

       const Ty* data() const { return &_data[0][0]; }
       Ty* data() { return &_data[0][0]; }
   };

    template<unsigned c,unsigned r>
    using IMatrix = Matrix<int,c,r>;

    template<unsigned c,unsigned r>
    using UMatrix = Matrix<unsigned int,c,r>;

    template<unsigned c,unsigned r>
    using FMatrix = Matrix<float,c,r>;

    template<unsigned c,unsigned r>
    using DMatrix = Matrix<double,c,r>;

    /// Single Row Matrices

    template<unsigned n>
    using IVec = IMatrix<n,1>;

    template<unsigned n>
    using UVec = UMatrix<n,1>;

    template<unsigned n>
    using FVec = FMatrix<n,1>;

    template<unsigned n>
    using DVec = DMatrix<n,1>;

    inline FVec<4> makeColor(float r,float g,float b,float a){
        auto m = FVec<4>::Create();
        m[0][0] = r;
        m[1][0] = g;
        m[2][0] = b;
        m[3][0] = a;
        return m;
    }

    // ==================================================================
    // Constants
    // ==================================================================

    template<class Ty> constexpr Ty Pi       = Ty(3.14159265358979323846);
    template<class Ty> constexpr Ty TwoPi    = Ty(6.28318530717958647692);
    template<class Ty> constexpr Ty HalfPi   = Ty(1.57079632679489661923);
    template<class Ty> constexpr Ty E        = Ty(2.71828182845904523536);
    template<class Ty> constexpr Ty Deg2Rad  = Pi<Ty> / Ty(180);
    template<class Ty> constexpr Ty Rad2Deg  = Ty(180) / Pi<Ty>;

    // ==================================================================
    // Determinant
    // ==================================================================

    template<class Ty>
    inline Ty determinant(const Matrix<Ty,2,2>& m){
        return m[0][0] * m[1][1] - m[0][1] * m[1][0];
    }

    template<class Ty>
    inline Ty determinant(const Matrix<Ty,3,3>& m){
        return m[0][0] * (m[1][1]*m[2][2] - m[1][2]*m[2][1])
             - m[0][1] * (m[1][0]*m[2][2] - m[1][2]*m[2][0])
             + m[0][2] * (m[1][0]*m[2][1] - m[1][1]*m[2][0]);
    }

    template<class Ty>
    inline Ty determinant(const Matrix<Ty,4,4>& m){
        // Laplace expansion along first row using 3x3 cofactors
        Ty s0 = m[0][0]*m[1][1] - m[1][0]*m[0][1];
        Ty s1 = m[0][0]*m[2][1] - m[2][0]*m[0][1];
        Ty s2 = m[0][0]*m[3][1] - m[3][0]*m[0][1];
        Ty s3 = m[1][0]*m[2][1] - m[2][0]*m[1][1];
        Ty s4 = m[1][0]*m[3][1] - m[3][0]*m[1][1];
        Ty s5 = m[2][0]*m[3][1] - m[3][0]*m[2][1];

        Ty c5 = m[2][2]*m[3][3] - m[3][2]*m[2][3];
        Ty c4 = m[1][2]*m[3][3] - m[3][2]*m[1][3];
        Ty c3 = m[1][2]*m[2][3] - m[2][2]*m[1][3];
        Ty c2 = m[0][2]*m[3][3] - m[3][2]*m[0][3];
        Ty c1 = m[0][2]*m[2][3] - m[2][2]*m[0][3];
        Ty c0 = m[0][2]*m[1][3] - m[1][2]*m[0][3];

        return s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0;
    }

    // ==================================================================
    // Inverse
    // ==================================================================

    template<class Ty>
    inline Matrix<Ty,2,2> inverse(const Matrix<Ty,2,2>& m){
        Ty det = determinant(m);
        Ty invDet = Ty(1) / det;
        auto r = Matrix<Ty,2,2>::Create();
        r[0][0] =  m[1][1] * invDet;
        r[0][1] = -m[0][1] * invDet;
        r[1][0] = -m[1][0] * invDet;
        r[1][1] =  m[0][0] * invDet;
        return r;
    }

    template<class Ty>
    inline Matrix<Ty,3,3> inverse(const Matrix<Ty,3,3>& m){
        Ty det = determinant(m);
        Ty invDet = Ty(1) / det;
        auto r = Matrix<Ty,3,3>::Create();
        r[0][0] = (m[1][1]*m[2][2] - m[1][2]*m[2][1]) * invDet;
        r[0][1] = (m[0][2]*m[2][1] - m[0][1]*m[2][2]) * invDet;
        r[0][2] = (m[0][1]*m[1][2] - m[0][2]*m[1][1]) * invDet;
        r[1][0] = (m[1][2]*m[2][0] - m[1][0]*m[2][2]) * invDet;
        r[1][1] = (m[0][0]*m[2][2] - m[0][2]*m[2][0]) * invDet;
        r[1][2] = (m[0][2]*m[1][0] - m[0][0]*m[1][2]) * invDet;
        r[2][0] = (m[1][0]*m[2][1] - m[1][1]*m[2][0]) * invDet;
        r[2][1] = (m[0][1]*m[2][0] - m[0][0]*m[2][1]) * invDet;
        r[2][2] = (m[0][0]*m[1][1] - m[0][1]*m[1][0]) * invDet;
        return r;
    }

    template<class Ty>
    inline Matrix<Ty,4,4> inverse(const Matrix<Ty,4,4>& m){
        Ty s0 = m[0][0]*m[1][1] - m[1][0]*m[0][1];
        Ty s1 = m[0][0]*m[2][1] - m[2][0]*m[0][1];
        Ty s2 = m[0][0]*m[3][1] - m[3][0]*m[0][1];
        Ty s3 = m[1][0]*m[2][1] - m[2][0]*m[1][1];
        Ty s4 = m[1][0]*m[3][1] - m[3][0]*m[1][1];
        Ty s5 = m[2][0]*m[3][1] - m[3][0]*m[2][1];

        Ty c5 = m[2][2]*m[3][3] - m[3][2]*m[2][3];
        Ty c4 = m[1][2]*m[3][3] - m[3][2]*m[1][3];
        Ty c3 = m[1][2]*m[2][3] - m[2][2]*m[1][3];
        Ty c2 = m[0][2]*m[3][3] - m[3][2]*m[0][3];
        Ty c1 = m[0][2]*m[2][3] - m[2][2]*m[0][3];
        Ty c0 = m[0][2]*m[1][3] - m[1][2]*m[0][3];

        Ty det = s0*c5 - s1*c4 + s2*c3 + s3*c2 - s4*c1 + s5*c0;
        Ty invDet = Ty(1) / det;

        auto r = Matrix<Ty,4,4>::Create();

        r[0][0] = ( m[1][1]*c5 - m[2][1]*c4 + m[3][1]*c3) * invDet;
        r[0][1] = (-m[0][1]*c5 + m[2][1]*c2 - m[3][1]*c1) * invDet;
        r[0][2] = ( m[0][1]*c4 - m[1][1]*c2 + m[3][1]*c0) * invDet;
        r[0][3] = (-m[0][1]*c3 + m[1][1]*c1 - m[2][1]*c0) * invDet;

        r[1][0] = (-m[1][0]*c5 + m[2][0]*c4 - m[3][0]*c3) * invDet;
        r[1][1] = ( m[0][0]*c5 - m[2][0]*c2 + m[3][0]*c1) * invDet;
        r[1][2] = (-m[0][0]*c4 + m[1][0]*c2 - m[3][0]*c0) * invDet;
        r[1][3] = ( m[0][0]*c3 - m[1][0]*c1 + m[2][0]*c0) * invDet;

        r[2][0] = ( m[1][3]*s5 - m[2][3]*s4 + m[3][3]*s3) * invDet;
        r[2][1] = (-m[0][3]*s5 + m[2][3]*s2 - m[3][3]*s1) * invDet;
        r[2][2] = ( m[0][3]*s4 - m[1][3]*s2 + m[3][3]*s0) * invDet;
        r[2][3] = (-m[0][3]*s3 + m[1][3]*s1 - m[2][3]*s0) * invDet;

        r[3][0] = (-m[1][2]*s5 + m[2][2]*s4 - m[3][2]*s3) * invDet;
        r[3][1] = ( m[0][2]*s5 - m[2][2]*s2 + m[3][2]*s1) * invDet;
        r[3][2] = (-m[0][2]*s4 + m[1][2]*s2 - m[3][2]*s0) * invDet;
        r[3][3] = ( m[0][2]*s3 - m[1][2]*s1 + m[2][2]*s0) * invDet;

        return r;
    }

    // ==================================================================
    // Transform matrix builders (float, 4x4)
    // ==================================================================

    inline FMatrix<4,4> translationMatrix(float x, float y, float z){
        auto m = FMatrix<4,4>::Identity();
        m[3][0] = x;
        m[3][1] = y;
        m[3][2] = z;
        return m;
    }

    inline FMatrix<4,4> scalingMatrix(float x, float y, float z){
        auto m = FMatrix<4,4>::Create();
        m[0][0] = x;
        m[1][1] = y;
        m[2][2] = z;
        m[3][3] = 1.f;
        return m;
    }

    inline FMatrix<4,4> rotationX(float radians){
        float c = cosf(radians), s = sinf(radians);
        auto m = FMatrix<4,4>::Identity();
        m[1][1] = c;  m[2][1] = -s;
        m[1][2] = s;  m[2][2] =  c;
        return m;
    }

    inline FMatrix<4,4> rotationY(float radians){
        float c = cosf(radians), s = sinf(radians);
        auto m = FMatrix<4,4>::Identity();
        m[0][0] =  c;  m[2][0] = s;
        m[0][2] = -s;  m[2][2] = c;
        return m;
    }

    inline FMatrix<4,4> rotationZ(float radians){
        float c = cosf(radians), s = sinf(radians);
        auto m = FMatrix<4,4>::Identity();
        m[0][0] = c;  m[1][0] = -s;
        m[0][1] = s;  m[1][1] =  c;
        return m;
    }

    inline FMatrix<4,4> rotationEuler(float pitch, float yaw, float roll){
        return rotationZ(roll) * rotationY(yaw) * rotationX(pitch);
    }

    // ==================================================================
    // Projection / View matrices
    // ==================================================================

    inline FMatrix<4,4> perspectiveProjection(float fovY, float aspect, float nearZ, float farZ){
        float tanHalf = tanf(fovY * 0.5f);
        auto m = FMatrix<4,4>::Create();
        m[0][0] = 1.f / (aspect * tanHalf);
        m[1][1] = 1.f / tanHalf;
        m[2][2] = farZ / (nearZ - farZ);
        m[2][3] = -1.f;
        m[3][2] = (nearZ * farZ) / (nearZ - farZ);
        return m;
    }

    inline FMatrix<4,4> orthographicProjection(float left, float right, float bottom, float top, float nearZ, float farZ){
        auto m = FMatrix<4,4>::Create();
        m[0][0] = 2.f / (right - left);
        m[1][1] = 2.f / (top - bottom);
        m[2][2] = 1.f / (nearZ - farZ);
        m[3][0] = -(right + left) / (right - left);
        m[3][1] = -(top + bottom) / (top - bottom);
        m[3][2] = nearZ / (nearZ - farZ);
        m[3][3] = 1.f;
        return m;
    }

    inline FMatrix<4,4> lookAt(const GPoint3D& eye, const GPoint3D& target, const GPoint3D& up){
        float fx = target.x - eye.x, fy = target.y - eye.y, fz = target.z - eye.z;
        float flen = sqrtf(fx*fx + fy*fy + fz*fz);
        fx /= flen; fy /= flen; fz /= flen;

        // side = normalize(cross(forward, up))
        float sx = fy*up.z - fz*up.y;
        float sy = fz*up.x - fx*up.z;
        float sz = fx*up.y - fy*up.x;
        float slen = sqrtf(sx*sx + sy*sy + sz*sz);
        sx /= slen; sy /= slen; sz /= slen;

        // recomputed up = cross(side, forward)
        float ux = sy*fz - sz*fy;
        float uy = sz*fx - sx*fz;
        float uz = sx*fy - sy*fx;

        auto m = FMatrix<4,4>::Identity();
        m[0][0] = sx;  m[1][0] = sy;  m[2][0] = sz;
        m[0][1] = ux;  m[1][1] = uy;  m[2][1] = uz;
        m[0][2] = -fx; m[1][2] = -fy; m[2][2] = -fz;
        m[3][0] = -(sx*eye.x + sy*eye.y + sz*eye.z);
        m[3][1] = -(ux*eye.x + uy*eye.y + uz*eye.z);
        m[3][2] =  (fx*eye.x + fy*eye.y + fz*eye.z);
        return m;
    }

    // ==================================================================
    // Viewport mapping
    // ==================================================================

    inline FMatrix<4,4> viewportMatrix(float width, float height, float farDepth){
        return scalingMatrix(2.f / width, 2.f / height, 2.f / farDepth);
    }

    // ==================================================================
    // Vector helpers (on FVec<N> = FMatrix<N,1>)
    // ==================================================================

    template<class Ty, unsigned N>
    inline Ty dot(const Matrix<Ty,N,1>& a, const Matrix<Ty,N,1>& b){
        Ty sum = 0;
        for(unsigned i = 0; i < N; i++) sum += a[i][0] * b[i][0];
        return sum;
    }

    template<class Ty>
    inline Matrix<Ty,3,1> cross(const Matrix<Ty,3,1>& a, const Matrix<Ty,3,1>& b){
        auto r = Matrix<Ty,3,1>::Create();
        r[0][0] = a[1][0]*b[2][0] - a[2][0]*b[1][0];
        r[1][0] = a[2][0]*b[0][0] - a[0][0]*b[2][0];
        r[2][0] = a[0][0]*b[1][0] - a[1][0]*b[0][0];
        return r;
    }

    template<class Ty, unsigned N>
    inline Ty length(const Matrix<Ty,N,1>& v){
        return std::sqrt(dot(v,v));
    }

    template<class Ty, unsigned N>
    inline Matrix<Ty,N,1> normalize(const Matrix<Ty,N,1>& v){
        Ty len = length(v);
        return v * (Ty(1) / len);
    }

    // ==================================================================
    // Point ↔ Matrix helpers
    // ==================================================================

    inline FVec<4> pointToVec4(const GPoint3D& pt, float w = 1.f){
        auto v = FVec<4>::Create();
        v[0][0] = pt.x; v[1][0] = pt.y; v[2][0] = pt.z; v[3][0] = w;
        return v;
    }

    inline GPoint3D vec4ToPoint(const FVec<4>& v){
        return GPoint3D{v[0][0], v[1][0], v[2][0]};
    }

    /// Applies `m` to `pt` under the column-major convention the transform
    /// builders above emit and the backends upload: element (row r, column c)
    /// is `m[c][r]`, translation lives in column 3, and the point is a column
    /// vector on the right — the CPU equivalent of a shader's
    /// `m * float4(pt, 1.0)`.
    ///
    /// Do NOT express this as `m * pointToVec4(pt)`: `Matrix::operator*`
    /// multiplies the raw storage as if the first index were the row, which for
    /// a column-major matrix applies the TRANSPOSE — it drops the translation
    /// column entirely and inverts every rotation.
    ///
    /// The homogeneous divide is applied when `w` is neither 0 nor 1, so a
    /// perspective matrix maps correctly too; for an affine matrix `w == 1` and
    /// the divide is a no-op.
    inline GPoint3D transformPoint(const FMatrix<4,4>& m, const GPoint3D& pt){
        const float p[4] = {pt.x, pt.y, pt.z, 1.f};
        float out[4] = {0.f, 0.f, 0.f, 0.f};
        for(unsigned r = 0; r < 4; r++)
            for(unsigned c = 0; c < 4; c++)
                out[r] += m[c][r] * p[c];

        const float w = out[3];
        if(w != 0.f && w != 1.f){
            out[0] /= w; out[1] /= w; out[2] /= w;
        }
        return GPoint3D{out[0], out[1], out[2]};
    }

    // ==================================================================
    // Quaternion
    // ==================================================================

    template<class Ty>
    struct Quaternion {
        Ty x, y, z, w;

        // --- Construction ---

        /// Returns the identity quaternion (0, 0, 0, 1).
        static Quaternion Identity(){
            return {Ty(0), Ty(0), Ty(0), Ty(1)};
        }

        /// Rotation of `radians` around the unit axis (ax, ay, az).
        static Quaternion fromAxisAngle(Ty ax, Ty ay, Ty az, Ty radians){
            Ty half = radians * Ty(0.5);
            Ty s = std::sin(half);
            return {ax * s, ay * s, az * s, std::cos(half)};
        }

        /// Equivalent to rotationEuler() but as a quaternion.
        /// Applies X (pitch) -> Y (yaw) -> Z (roll), matching the existing convention.
        static Quaternion fromEuler(Ty pitch, Ty yaw, Ty roll){
            Ty cx = std::cos(pitch * Ty(0.5)), sx = std::sin(pitch * Ty(0.5));
            Ty cy = std::cos(yaw   * Ty(0.5)), sy = std::sin(yaw   * Ty(0.5));
            Ty cz = std::cos(roll  * Ty(0.5)), sz = std::sin(roll  * Ty(0.5));
            return {
                sx*cy*cz - cx*sy*sz,
                cx*sy*cz + sx*cy*sz,
                cx*cy*sz - sx*sy*cz,
                cx*cy*cz + sx*sy*sz
            };
        }

        /// Extracts the rotation from a 4x4 matrix (upper-left 3x3).
        /// Uses Shepperd's method for numerical stability.
        static Quaternion fromMatrix(const Matrix<Ty,4,4>& m){
            Ty m00 = m[0][0], m11 = m[1][1], m22 = m[2][2];
            Ty trace = m00 + m11 + m22;
            Ty qx, qy, qz, qw;

            if(trace > Ty(0)){
                Ty s = std::sqrt(trace + Ty(1)) * Ty(2); // s = 4*w
                qw = Ty(0.25) * s;
                qx = (m[1][2] - m[2][1]) / s;
                qy = (m[2][0] - m[0][2]) / s;
                qz = (m[0][1] - m[1][0]) / s;
            } else if(m00 > m11 && m00 > m22){
                Ty s = std::sqrt(Ty(1) + m00 - m11 - m22) * Ty(2); // s = 4*x
                qw = (m[1][2] - m[2][1]) / s;
                qx = Ty(0.25) * s;
                qy = (m[0][1] + m[1][0]) / s;
                qz = (m[2][0] + m[0][2]) / s;
            } else if(m11 > m22){
                Ty s = std::sqrt(Ty(1) + m11 - m00 - m22) * Ty(2); // s = 4*y
                qw = (m[2][0] - m[0][2]) / s;
                qx = (m[0][1] + m[1][0]) / s;
                qy = Ty(0.25) * s;
                qz = (m[1][2] + m[2][1]) / s;
            } else {
                Ty s = std::sqrt(Ty(1) + m22 - m00 - m11) * Ty(2); // s = 4*z
                qw = (m[0][1] - m[1][0]) / s;
                qx = (m[2][0] + m[0][2]) / s;
                qy = (m[1][2] + m[2][1]) / s;
                qz = Ty(0.25) * s;
            }
            return {qx, qy, qz, qw};
        }

        // --- Arithmetic ---

        /// Hamilton product. Composes rotations: (q1 * q2) applies q2 first, then q1.
        Quaternion operator*(const Quaternion& o) const {
            return {
                w*o.x + x*o.w + y*o.z - z*o.y,
                w*o.y - x*o.z + y*o.w + z*o.x,
                w*o.z + x*o.y - y*o.x + z*o.w,
                w*o.w - x*o.x - y*o.y - z*o.z
            };
        }

        Quaternion operator*(Ty scalar) const {
            return {x*scalar, y*scalar, z*scalar, w*scalar};
        }
        friend Quaternion operator*(Ty scalar, const Quaternion& q){
            return q * scalar;
        }

        Quaternion operator+(const Quaternion& o) const {
            return {x+o.x, y+o.y, z+o.z, w+o.w};
        }
        Quaternion operator-(const Quaternion& o) const {
            return {x-o.x, y-o.y, z-o.z, w-o.w};
        }
        Quaternion operator-() const {
            return {-x, -y, -z, -w};
        }

        // --- Operations ---

        Ty lengthSquared() const { return x*x + y*y + z*z + w*w; }
        Ty length() const { return std::sqrt(lengthSquared()); }

        Quaternion normalized() const {
            Ty len = length();
            return {x/len, y/len, z/len, w/len};
        }

        Quaternion conjugate() const { return {-x, -y, -z, w}; }

        Quaternion inverse() const {
            Ty lenSq = lengthSquared();
            return {-x/lenSq, -y/lenSq, -z/lenSq, w/lenSq};
        }

        Ty dot(const Quaternion& o) const {
            return x*o.x + y*o.y + z*o.z + w*o.w;
        }

        // --- Conversion ---

        /// Produces a 4x4 rotation matrix (no translation/scale).
        Matrix<Ty,4,4> toMatrix() const {
            Ty xx = x*x, yy = y*y, zz = z*z;
            Ty xy = x*y, xz = x*z, yz = y*z;
            Ty wx = w*x, wy = w*y, wz = w*z;

            auto m = Matrix<Ty,4,4>::Create();
            m[0][0] = Ty(1) - Ty(2)*(yy + zz);
            m[0][1] = Ty(2)*(xy + wz);
            m[0][2] = Ty(2)*(xz - wy);

            m[1][0] = Ty(2)*(xy - wz);
            m[1][1] = Ty(1) - Ty(2)*(xx + zz);
            m[1][2] = Ty(2)*(yz + wx);

            m[2][0] = Ty(2)*(xz + wy);
            m[2][1] = Ty(2)*(yz - wx);
            m[2][2] = Ty(1) - Ty(2)*(xx + yy);

            m[3][3] = Ty(1);
            return m;
        }

        // --- Interpolation ---

        /// Normalized linear interpolation. Cheaper than slerp, nearly identical
        /// for small angular differences.
        static Quaternion nlerp(const Quaternion& a, const Quaternion& b, Ty t){
            Quaternion target = (a.dot(b) < Ty(0)) ? -b : b;
            return (a * (Ty(1) - t) + target * t).normalized();
        }

        /// Spherical linear interpolation. t=0 returns a, t=1 returns b.
        /// Follows the shortest arc (flips b if dot(a,b) < 0).
        static Quaternion slerp(const Quaternion& a, const Quaternion& b, Ty t){
            Ty cosTheta = a.dot(b);
            Quaternion target = b;
            if(cosTheta < Ty(0)){
                target = -b;
                cosTheta = -cosTheta;
            }
            if(cosTheta > Ty(0.9995)){
                return nlerp(a, target, t);
            }
            Ty theta = std::acos(cosTheta);
            Ty sinTheta = std::sin(theta);
            Ty wa = std::sin((Ty(1) - t) * theta) / sinTheta;
            Ty wb = std::sin(t * theta) / sinTheta;
            return a * wa + target * wb;
        }
    };

    using FQuaternion = Quaternion<float>;

    // ==================================================================
    // Quaternion free functions
    // ==================================================================

    /// Rotate a point by a quaternion (optimized q * p * q_inverse).
    template<class Ty>
    inline GPoint3D rotatePoint(const Quaternion<Ty>& q, const GPoint3D& pt){
        // t = 2 * cross(q.xyz, pt)
        Ty tx = Ty(2) * (q.y * pt.z - q.z * pt.y);
        Ty ty = Ty(2) * (q.z * pt.x - q.x * pt.z);
        Ty tz = Ty(2) * (q.x * pt.y - q.y * pt.x);
        // result = pt + w*t + cross(q.xyz, t)
        return {
            pt.x + q.w * tx + (q.y * tz - q.z * ty),
            pt.y + q.w * ty + (q.z * tx - q.x * tz),
            pt.z + q.w * tz + (q.x * ty - q.y * tx)
        };
    }

    /// Build a lookAt-style quaternion (camera orientation).
    inline FQuaternion lookAtQuaternion(const GPoint3D& forward, const GPoint3D& up){
        // Normalize forward
        float flen = std::sqrt(forward.x*forward.x + forward.y*forward.y + forward.z*forward.z);
        float fx = forward.x/flen, fy = forward.y/flen, fz = forward.z/flen;

        // side = normalize(cross(forward, up))
        float sx = fy*up.z - fz*up.y;
        float sy = fz*up.x - fx*up.z;
        float sz = fx*up.y - fy*up.x;
        float slen = std::sqrt(sx*sx + sy*sy + sz*sz);
        sx /= slen; sy /= slen; sz /= slen;

        // recomputed up = cross(side, forward)
        float ux = sy*fz - sz*fy;
        float uy = sz*fx - sx*fz;
        float uz = sx*fy - sy*fx;

        // Build 3x3 rotation matrix and extract quaternion
        // Row 0: side,  Row 1: up,  Row 2: -forward
        auto m = FMatrix<4,4>::Identity();
        m[0][0] = sx;  m[0][1] = ux;  m[0][2] = -fx;
        m[1][0] = sy;  m[1][1] = uy;  m[1][2] = -fy;
        m[2][0] = sz;  m[2][1] = uz;  m[2][2] = -fz;
        return FQuaternion::fromMatrix(m);
    }

_NAMESPACE_END_

namespace OmegaCommon {
    /// @brief Formatter of FVector2D
    template<>
    struct FormatProvider<OmegaGTE::FVector2D> {
        static void format(std::ostream & out,OmegaGTE::FVector2D & object){
            out << "OmegaGTE.FVector2D {" << object.getI() << "i, " << object.getJ() << "j}" << std::flush;
        }
    };

    template<>
    struct FormatProvider<OmegaGTE::FVector3D> {
        static void format(std::ostream & out,OmegaGTE::FVector3D & object){
            out << "OmegaGTE.FVector3D {" << object.getI() << "i, " << object.getJ() << "j, " << object.getK() << "k}" << std::flush;
        }
    };

    template<>
    struct FormatProvider<OmegaGTE::FVec<2>>{
        static void format(std::ostream & out,OmegaGTE::FVec<2> & object){
            out << "OmegaGTE.FVec<2> [" << object[0][0] << "," << object[1][0] << "]";
        }
    };

    template<>
    struct FormatProvider<OmegaGTE::FVec<3>>{
        static void format(std::ostream & out,OmegaGTE::FVec<3> & object){
            out << "OmegaGTE.FVec<3> [" << object[0][0] << "," << object[1][0] << "," << object[2][0] <<  "]";
        }
    };

    template<>
    struct FormatProvider<OmegaGTE::FVec<4>>{
        static void format(std::ostream & out,OmegaGTE::FVec<4> & object){
            out << "OmegaGTE.FVec<4> [" << object[0][0] << "," << object[1][0] << "," << object[2][0] << "," << object[3][0] << "]";
        }
    };
}

#endif
