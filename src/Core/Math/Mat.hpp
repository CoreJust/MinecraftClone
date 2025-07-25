#pragma once
#include "Vec.hpp"

namespace core {
    template<usize Rows_, usize Cols_, typename T>
    struct Mat {
        constexpr static inline usize Rows = Rows_;
        constexpr static inline usize Cols = Cols_;
        constexpr static inline usize Size = Rows * Cols;
        constexpr static inline bool IsSquare = (Rows == Cols);
        static_assert(Rows != 0 && Cols != 0);

        using Row = Vec<Cols, T>;

        T data[Rows * Cols] { };

        PURE constexpr static Mat const& Identity() noexcept {
            constexpr static Mat result = ([] consteval {
                Mat result { };
                for (usize row = 0; row < Rows; ++row) {
                    for (usize col = 0; col < Cols; ++col)
                        result.data[row * Cols + col] = static_cast<T>(row == col);
                }
                return result;
            })();
            return result;
        }
        
        PURE constexpr bool operator==(Mat const&) const noexcept = default;

        PURE INLINE constexpr Row& operator[](usize idx) noexcept {
            ASSERT(idx >= 0 && static_cast<usize>(idx) < Rows);
            return *rowPtr(idx);
        }

        PURE INLINE constexpr Row const& operator[](usize idx) const noexcept {
            ASSERT(idx >= 0 && static_cast<usize>(idx) < Rows);
            return *rowPtr(idx);
        }

        PURE INLINE constexpr Row      * rowPtr(usize idx)       noexcept { return reinterpret_cast<Row*>(&data[idx * Cols]); }
        PURE INLINE constexpr Row const* rowPtr(usize idx) const noexcept { return reinterpret_cast<Row const*>(&data[idx * Cols]); }

        PURE INLINE constexpr Mat operator*(Mat const& rhs) const noexcept {
            Mat b = rhs.transposed();
            Mat result { };
            Row const* rowA = rowPtr(0);
            Row const* endA = rowPtr(Rows);
            Row* rowC = result.rowPtr(0);
            while (rowA < endA) {
                Row const* rowB = b.rowPtr(0);
                Row const* endB = b.rowPtr(Rows);
                T* elemC = rowC->elemPtr(0);
                while (rowB < endB) {
                    T const* elemB = rowB->data;
                    T const* elemA = rowA->data;
                    T const* endElemA = elemA + Cols;
                    T tmp = static_cast<T>(0);
                    while (elemA < endElemA)
                        tmp += *(elemA++) * *(elemB++);
                    *(elemC++) = tmp;
                    ++rowB;
                }
                ++rowA;
                ++rowC;
            }
            return result;
        }

        PURE constexpr Mat& operator*=(Mat const& rhs) &noexcept {
            return *this = *this * rhs;
        }

        PURE constexpr Row operator*(Row const& vec) const noexcept {
            Row result { };
            T* dst = result.begin();
            T* end = result.end();
            Row const* rowSrc = begin();
            while (dst < end) {
                T tmp = static_cast<T>(0);
                T const* a = (rowSrc++)->begin();
                T const* b = vec.begin();
                T const* bEnd = vec.end();
                while (b < bEnd)
                    tmp += *(a++) * *(b++);
                *(dst++) = tmp;
            }
            return result;
        }

#define DECL_MAT_EQ_OP(op)                                             \
        PURE constexpr Mat& operator op##=(Mat const& rhs) &noexcept { \
            T* dst = data, *src = rhs.data, *end = data + Size;        \
            while (dst < end) *(dst++) op##= *(src++);                 \
            return *this;                                              \
        }
        DECL_MAT_EQ_OP(+)
        DECL_MAT_EQ_OP(-)
#undef DECL_MAT_EQ_OP
#define DECL_SCALAR_EQ_OP(op)                                 \
        PURE constexpr Mat& operator op##=(T rhs) &noexcept { \
            T* dst = data, *end = data + Size;                \
            while (dst < end) *(dst++) op##= rhs;             \
            return *this;                                     \
        }
        DECL_SCALAR_EQ_OP(+)
        DECL_SCALAR_EQ_OP(-)
        DECL_SCALAR_EQ_OP(*)
        DECL_SCALAR_EQ_OP(/)
        DECL_SCALAR_EQ_OP(%)
#undef DECL_SCALAR_EQ_OP
#define DECL_MAT_OP(op, rhsTy) \
        PURE constexpr Mat operator op(rhsTy rhs) const noexcept { Mat tmp = *this; tmp op##= rhs; return tmp; }

        DECL_MAT_OP(+, Mat const&)
        DECL_MAT_OP(-, Mat const&)
        DECL_MAT_OP(+, T)
        DECL_MAT_OP(-, T)
        DECL_MAT_OP(*, T)
        DECL_MAT_OP(/, T)
        DECL_MAT_OP(%, T)
#undef DECL_MAT_OP

        template<typename U>
        PURE constexpr Mat<Rows, Cols, U> to() const noexcept {
            Mat<Rows, Cols, U> result { };
            T* dst = result.data, *src = data, *end = data + Size;
            while (src < end) *(dst++) = static_cast<U>(*(src++));
            return result;
        }

        template<usize NewRows, usize NewCols, usize FromRow = 0, usize FromCol = 0>
        PURE constexpr Mat<NewRows, NewCols, T> block() const noexcept {
            static_assert(NewRows + FromRow <= Rows);
            static_assert(NewCols + FromCol <= Cols);
            Mat<NewRows, NewCols, T> result { };
            Row* dst = &result[0];
            Row const* src = &(*this)[FromRow];
            Row const* end = rowPtr(FromRow + NewRows);
            while (src < end) *(dst++) = (src++)->template slice<NewCols, FromCol>();
            return result;
        }

        PURE constexpr Mat<Cols, Rows, T> transposed() const noexcept {
            Mat<Cols, Rows, T> result { };
            T* dst = result.data;
            T const* srcCol = data;
            T const* srcColEnd = data + Rows;
            while (srcCol < srcColEnd) {
                T const* src = srcCol;
                T const* end = src + Size;
                while (src < end) {
                    *(dst++) = *src;
                    src += Rows;
                }
                ++srcCol;
            }
            return result;
        }

        PURE constexpr Vec<Size, T>      & flat()       noexcept { return *reinterpret_cast<Vec<Size, T>*>(data); }
        PURE constexpr Vec<Size, T> const& flat() const noexcept { return *reinterpret_cast<Vec<Size, T>*>(data); }
        PURE constexpr Row      * begin ()       noexcept { return rowPtr(0); }
        PURE constexpr Row const* begin () const noexcept { return rowPtr(0); }
        PURE constexpr Row const* cbegin() const noexcept { return rowPtr(0); }
        PURE constexpr Row      * end   ()       noexcept { return rowPtr(Rows); }
        PURE constexpr Row const* end   () const noexcept { return rowPtr(Rows); }
        PURE constexpr Row const* cend  () const noexcept { return rowPtr(Rows); }
    };
    
#define DECL_MAT_TYPES(n)                          \
    template<typename T> using Mat##n = Mat<n, n, T>; \
    using Mat##n##i8  = Mat<n, n, i8>;                \
    using Mat##n##i16 = Mat<n, n, i16>;               \
    using Mat##n##i32 = Mat<n, n, i32>;               \
    using Mat##n##i64 = Mat<n, n, i64>;               \
    using Mat##n##u8  = Mat<n, n, u8>;                \
    using Mat##n##u16 = Mat<n, n, u16>;               \
    using Mat##n##u32 = Mat<n, n, u32>;               \
    using Mat##n##u64 = Mat<n, n, u64>;               \
    using Mat##n##f   = Mat<n, n, float>;             \
    using Mat##n##d   = Mat<n, n, double>;
    DECL_MAT_TYPES(2)
    DECL_MAT_TYPES(3)
    DECL_MAT_TYPES(4)
#undef DECL_MAT_TYPES
} // namespace core
