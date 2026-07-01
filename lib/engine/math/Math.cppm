module;

#include <mango/math/math.hpp>

#include "pP/Macros.h"

export module engine.math;

export namespace mango::math {}

// warning C5304: a declaration designated by the using-declaration 'XXX' exported from this module has internal linkage and using such a name outside the module is ill-formed
#ifdef _MSC_VER
#pragma warning(disable :  5304)
#endif

export namespace math {
    using namespace mango::math;

    using float2 = mango::math::float32x2;
    using float3 = mango::math::float32x3;
    using float4 = mango::math::float32x4;

    using float3x3 = mango::math::Matrix3x3;
    using float4x4 = mango::math::Matrix4x4;

    inline float4x4 translate(const float4x4& matrix, const float x, const float y, const float z) { return mango::math::translate(matrix, x, y, z); }
    inline float4x4 scale(const float4x4& matrix, const float s) { return mango::math::scale(matrix, s); }
    inline float4x4 scale(const float4x4& matrix, const float x, const float y, const float z) { return mango::math::scale(matrix, x, y, z); }
    inline float4x4 rotate(const float4x4& matrix, const float angle, const float3& axis) { return mango::math::rotate(matrix, angle, axis); }
    inline float4x4 rotateX(const float4x4& matrix, const float angle) { return mango::math::rotateX(matrix, angle); }
    inline float4x4 rotateY(const float4x4& matrix, const float angle) { return mango::math::rotateY(matrix, angle); }
    inline float4x4 rotateZ(const float4x4& matrix, const float angle) { return mango::math::rotateZ(matrix, angle); }
    inline float4x4 rotateXYZ(const float4x4& matrix, const float x, const float y, const float z) { return mango::math::rotateXYZ(matrix, x, y, z); }
    inline float4x4 mirror(const float4x4& matrix, const float4& plane) { return mango::math::mirror(matrix, plane); }
    inline float4x4 affineInverse(const float4x4& matrix) { return mango::math::affineInverse(matrix); }
    inline float4x4 adjoint(const float4x4& matrix) { return mango::math::adjoint(matrix); }

    inline float4x4 oblique(const float4x4& matrix, const float4& nearClip) { return mango::math::obliqueD3D(matrix, nearClip); }

    using mango::math::Quaternion;
    using mango::math::AngleAxis;
    using mango::math::EulerAngles;

    using mango::math::Quadratic;
    using mango::math::LineSegment;
    using mango::math::Ray;
    using mango::math::FastRay;
    using mango::math::Rectangle;
    using mango::math::Plane;
    using mango::math::Box;
    using mango::math::Sphere;
    using mango::math::Triangle;
    using mango::math::Frustum;
    using mango::math::RayFrustum;

    using mango::math::Intersect;
    using mango::math::IntersectRange;
    using mango::math::IntersectSolid;
    using mango::math::IntersectBarycentric;
    using mango::math::IntersectBarycentricTwosided;

    using mango::math::intersect;

    template <typename T>
        requires requires (T x)
    {
        mango::math::dot(x, x);
    }
    inline auto dot2(T x) {
        return dot(x, x);
    }

    template <typename T>
    [[nodiscard]] auto perspectiveD3D(T fovY, T aspect, T nearZ, T farZ) {
        return mango::math::Matrix4x4::perspectiveD3D(fovY * aspect, fovY, nearZ, farZ);
    }

    [[nodiscard]] inline float4x4 operator*(const float4x4& a, const float4x4& b) {
        return mango::math::operator*(a, b);
    }

    [[nodiscard]] inline float4 operator*(const float4& v, const float4x4& m) {
        return mango::math::operator*(v, m);
    }

    [[nodiscard]] inline float3 operator-(const float3& a, const float3& b) {
        return mango::math::operator-(a, b);
    }

    [[nodiscard]] inline float4x4 inverse(const float4x4& m) {
        return mango::math::inverse(m);
    }

    template <typename T>
    [[nodiscard]] inline auto normalize(const T& v) {
        return mango::math::normalize(v);
    }

    [[nodiscard]] inline float4x4 lookAt(const float3& viewer, const float3& target, const float3& up) {
        return mango::math::Matrix4x4::lookat(target, viewer, up);
    }

}




