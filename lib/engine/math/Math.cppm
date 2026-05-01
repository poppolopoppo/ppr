module;

#include <mango/math/math.hpp>

#include "pP/Macros.h"

export module engine.math;

export namespace mango::math {}

// warning C5304: a declaration designated by the using-declaration 'XXX' exported from this module has internal linkage and using such a name outside the module is ill-formed
#pragma warning(disable :  5304)

export namespace math {
    using namespace mango::math;

    using mango::math::pi;

    using float2 = mango::math::float32x2;
    using float3 = mango::math::float32x3;
    using float4 = mango::math::float32x4;

    using float3x3 = mango::math::Matrix3x3;
    using float4x4 = mango::math::Matrix4x4;

    // ------------------------------------------------------------------
    // scalar functions
    // ------------------------------------------------------------------

    using mango::math::is_scalar;

    using mango::math::abs;
    using mango::math::sqrt;
    using mango::math::sin;
    using mango::math::cos;
    using mango::math::tan;
    using mango::math::asin;
    using mango::math::acos;
    using mango::math::atan;
    using mango::math::exp;
    using mango::math::log;
    using mango::math::exp2;
    using mango::math::log2;
    using mango::math::pow;
    using mango::math::atan2;
    using mango::math::round;
    using mango::math::floor;
    using mango::math::ceil;
    using mango::math::trunc;
    using mango::math::fract;
    using mango::math::mod;
    using mango::math::min;
    using mango::math::max;
    using mango::math::clamp;
    using mango::math::lerp;
    using mango::math::smoothstep;
    using mango::math::sign;
    using mango::math::radians;
    using mango::math::degrees;

    // -----------------------------------------------------------------
    // vectors
    // -----------------------------------------------------------------

    using mango::math::dot;
    using mango::math::square;
    using mango::math::distance;
    using mango::math::normalize;
    using mango::math::project;
    using mango::math::reflect;
    using mango::math::refract;
    using mango::math::cross;
    using mango::math::hmin;
    using mango::math::hmax;
    using mango::math::clamp;
    using mango::math::smoothstep;

    using mango::math::unpacklo;
    using mango::math::unpackhi;

    using mango::math::adds;
    using mango::math::subs;

    using mango::math::add;
    using mango::math::sub;
    using mango::math::mul;
    using mango::math::div;

    // ------------------------------------------------------------------
    // matrices
    // ------------------------------------------------------------------

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

    using mango::math::transpose;
    using mango::math::inverse;
    using mango::math::inverseTranspose;
    using mango::math::inverseTRS;
    using mango::math::inverseTR;

    // -----------------------------------------------------------------
    // quaternion
    // -----------------------------------------------------------------

    using mango::math::Quaternion;
    using mango::math::AngleAxis;
    using mango::math::EulerAngles;

    using mango::math::dot;
    using mango::math::norm;
    using mango::math::square;
    using mango::math::mod;
    using mango::math::negate;
    using mango::math::inverse;
    using mango::math::conjugate;

    using mango::math::log;
    using mango::math::exp;
    using mango::math::pow;
    using mango::math::normalize;
    using mango::math::lndif;
    using mango::math::lerp;
    using mango::math::slerp;
    using mango::math::squad;

    // -----------------------------------------------------------------
    // geometry
    // -----------------------------------------------------------------

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

    // ------------------------------------------------------------------
    // easing functions
    // ------------------------------------------------------------------

    // Illustration of the ease functions:
    // https://easings.net/

    using mango::math::easeLinear;
    using mango::math::easeSmoothStep;
    using mango::math::easeInQuadratic;
    using mango::math::easeOutQuadratic;
    using mango::math::easeInOutQuadratic;

    using mango::math::easeInCubic;
    using mango::math::easeOutCubic;
    using mango::math::easeInOutCubic;

    using mango::math::easeInQuartic;
    using mango::math::easeOutQuartic;
    using mango::math::easeInOutQuartic;

    using mango::math::easeInQuintic;
    using mango::math::easeOutQuintic;
    using mango::math::easeInOutQuintic;

    using mango::math::easeInSine;
    using mango::math::easeOutSine;
    using mango::math::easeInOutSine;

    using mango::math::easeInCircular;
    using mango::math::easeOutCircular;
    using mango::math::easeInOutCircular;

    using mango::math::easeInExponential;
    using mango::math::easeOutExponential;
    using mango::math::easeInOutExponential;

    using mango::math::easeInElastic;
    using mango::math::easeOutElastic;
    using mango::math::easeInOutElastic;

    using mango::math::easeInBack;
    using mango::math::easeOutBack;
    using mango::math::easeInOutBack;

    using mango::math::easeInBounce;
    using mango::math::easeOutBounce;
    using mango::math::easeInOutBounce;

    // ------------------------------------------------------------------
    // spline interpolation
    // ------------------------------------------------------------------

    using mango::math::bezier;
    using mango::math::catmull;
    using mango::math::bicubic;
    using mango::math::bspline;
    using mango::math::hermite;

    // ------------------------------------------------------------------
    // colors
    // ------------------------------------------------------------------

    using mango::math::linear_to_srgb;
    using mango::math::srgb_to_linear;

    // ------------------------------------------------------------------
    // helpers
    // ------------------------------------------------------------------

    template <typename T>
        requires requires (T x)
    {
        mango::math::dot(x, x);
    }
    inline auto dot2(T x) {
        return dot(x, x);
    }

}

// ------------------------------------------------------------------
// operators (for ADL)
// ------------------------------------------------------------------

export using mango::math::operator+;
export using mango::math::operator-;
export using mango::math::operator*;
export using mango::math::operator/;
export using mango::math::operator+=;
export using mango::math::operator-=;
export using mango::math::operator*=;
export using mango::math::operator/=;
export using mango::math::operator==;
export using mango::math::operator!=;
export using mango::math::operator<;
export using mango::math::operator<=;
export using mango::math::operator>;
export using mango::math::operator>=;
export using mango::math::operator^;
export using mango::math::operator~;
export using mango::math::operator&;
export using mango::math::operator|;
export using mango::math::operator<<;
export using mango::math::operator>>;
