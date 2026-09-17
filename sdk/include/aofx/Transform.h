// Copyright (c) 2026 aopenfx contributors.
//
// The arithmetic of a transform: Nuke's, written down once.
//
// ONE COPY, IN THE SDK
//
// Before this file there were three implementations of "what does `rotate`
// mean": the renderer's, the viewer's and the tests' hand-computed numbers,
// and they had to agree by inspection. Parenting, pivots and cameras read from
// files multiply the places that need the answer, so it lives here, in the SDK,
// where the plugin -- which links the SDK headers and nothing else -- can reach
// it as well as the host.
//
// Header-only and inline: nothing crosses the plugin boundary, so it is not
// part of the ABI. `Version.h`'s ledger says so.
//
// CONVENTIONS, STATED
//
// Right-handed. Matrices are 4x4 row-major and act on column vectors, so a
// point transforms as `p' = M * p` and a child's world matrix is
// `parent * local`. Angles are degrees. A rotation order is spelled the way
// Nuke spells it: the letters are the matrix product left to right, so ZXY is
// `Rz * Rx * Ry` and the rightmost is applied to the point first. Nuke's
// default is ZXY; this project's behaviour before there was a choice was ZYX.
//
// A transform order is spelled in the order the operations are applied to the
// point: SRT means scale, then rotate, then translate, so the matrix product is
// `T * R * S`. Nuke's default, and Nuke's documented form with a pivot is
// `T(t) * T(p) * R * S * T(-p)`, which is what this produces.
#pragma once

#include <array>
#include <string_view>
#include <cmath>
#include <cstddef>

namespace aofx::xform {

constexpr double kPi = 3.14159265358979323846;
constexpr double kToRadians = kPi / 180.0;
constexpr double kToDegrees = 180.0 / kPi;

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline double length(const Vec3& a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalised(const Vec3& a) {
    const double l = length(a);
    return l > 1e-12 ? a * (1.0 / l) : Vec3{0.0, 0.0, 0.0};
}

/// 4x4, row-major: `m[row * 4 + column]`. The last row is 0 0 0 1 for
/// everything this file makes, and `point` relies on it.
struct Mat4 {
    std::array<double, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Mat4 identity() { return {}; }

    [[nodiscard]] double at(int row, int column) const { return m[static_cast<size_t>(row * 4 + column)]; }
    double& at(int row, int column) { return m[static_cast<size_t>(row * 4 + column)]; }

    [[nodiscard]] Vec3 point(const Vec3& p) const {
        return {at(0, 0) * p.x + at(0, 1) * p.y + at(0, 2) * p.z + at(0, 3),
                at(1, 0) * p.x + at(1, 1) * p.y + at(1, 2) * p.z + at(1, 3),
                at(2, 0) * p.x + at(2, 1) * p.y + at(2, 2) * p.z + at(2, 3)};
    }
    /// A direction: rotated and scaled, never translated.
    [[nodiscard]] Vec3 direction(const Vec3& d) const {
        return {at(0, 0) * d.x + at(0, 1) * d.y + at(0, 2) * d.z,
                at(1, 0) * d.x + at(1, 1) * d.y + at(1, 2) * d.z,
                at(2, 0) * d.x + at(2, 1) * d.y + at(2, 2) * d.z};
    }
    [[nodiscard]] Vec3 translation() const { return {at(0, 3), at(1, 3), at(2, 3)}; }
    [[nodiscard]] Vec3 column(int c) const { return {at(0, c), at(1, c), at(2, c)}; }

    /// The upper 3x4 as twelve floats, row-major -- what a kernel takes.
    [[nodiscard]] std::array<float, 12> rows3x4() const {
        std::array<float, 12> out{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                out[static_cast<size_t>(r * 4 + c)] = static_cast<float>(at(r, c));
            }
        }
        return out;
    }
};

inline Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += a.at(r, k) * b.at(k, c);
            }
            out.at(r, c) = sum;
        }
    }
    return out;
}

inline Mat4 translation(const Vec3& t) {
    Mat4 out;
    out.at(0, 3) = t.x;
    out.at(1, 3) = t.y;
    out.at(2, 3) = t.z;
    return out;
}

inline Mat4 scaling(const Vec3& s) {
    Mat4 out;
    out.at(0, 0) = s.x;
    out.at(1, 1) = s.y;
    out.at(2, 2) = s.z;
    return out;
}

inline Mat4 rotationX(double degrees) {
    const double c = std::cos(degrees * kToRadians);
    const double s = std::sin(degrees * kToRadians);
    Mat4 out;
    out.at(1, 1) = c; out.at(1, 2) = -s;
    out.at(2, 1) = s; out.at(2, 2) = c;
    return out;
}
inline Mat4 rotationY(double degrees) {
    const double c = std::cos(degrees * kToRadians);
    const double s = std::sin(degrees * kToRadians);
    Mat4 out;
    out.at(0, 0) = c;  out.at(0, 2) = s;
    out.at(2, 0) = -s; out.at(2, 2) = c;
    return out;
}
inline Mat4 rotationZ(double degrees) {
    const double c = std::cos(degrees * kToRadians);
    const double s = std::sin(degrees * kToRadians);
    Mat4 out;
    out.at(0, 0) = c; out.at(0, 1) = -s;
    out.at(1, 0) = s; out.at(1, 1) = c;
    return out;
}

/// Nuke's three shears: x += k.x * y, x += k.y * z, y += k.z * z.
inline Mat4 skewing(const Vec3& k) {
    Mat4 out;
    out.at(0, 1) = k.x;
    out.at(0, 2) = k.y;
    out.at(1, 2) = k.z;
    return out;
}

/// Letters are the matrix product, left to right; the rightmost axis is
/// applied to the point first. ZXY is Nuke's default. ZYX is what this project
/// did before it had a choice -- `Rz * Ry * Rx`, X applied first.
enum class RotationOrder : unsigned char { XYZ, XZY, YXZ, YZX, ZXY, ZYX };

/// The order the operations are applied to the point. SRT -- scale, then
/// rotate, then translate -- is Nuke's default.
enum class TransformOrder : unsigned char { SRT, STR, RST, RTS, TSR, TRS };

inline const char* toString(RotationOrder order) {
    switch (order) {
        case RotationOrder::XYZ: return "XYZ";
        case RotationOrder::XZY: return "XZY";
        case RotationOrder::YXZ: return "YXZ";
        case RotationOrder::YZX: return "YZX";
        case RotationOrder::ZXY: return "ZXY";
        case RotationOrder::ZYX: return "ZYX";
    }
    return "ZXY";
}
inline const char* toString(TransformOrder order) {
    switch (order) {
        case TransformOrder::SRT: return "SRT";
        case TransformOrder::STR: return "STR";
        case TransformOrder::RST: return "RST";
        case TransformOrder::RTS: return "RTS";
        case TransformOrder::TSR: return "TSR";
        case TransformOrder::TRS: return "TRS";
    }
    return "SRT";
}

/// The three axis indices of an order, product order left to right.
/// Three letters name an order whatever their case: "zxy" on a knob, "ZXY"
/// from `toString`.
inline bool sameOrderKey(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = a[i] >= 'a' && a[i] <= 'z' ? static_cast<char>(a[i] - 'a' + 'A') : a[i];
        const char y = b[i] >= 'a' && b[i] <= 'z' ? static_cast<char>(b[i] - 'a' + 'A') : b[i];
        if (x != y) {
            return false;
        }
    }
    return true;
}

/// The order a knob names, by its key -- "zxy", "srt". Nothing for a key that
/// is not one, so a caller can fall back to the default it wants.
inline bool rotationOrderFrom(std::string_view key, RotationOrder& out) {
    for (const RotationOrder order : {RotationOrder::XYZ, RotationOrder::XZY, RotationOrder::YXZ,
                                      RotationOrder::YZX, RotationOrder::ZXY, RotationOrder::ZYX}) {
        if (sameOrderKey(key, toString(order))) {
            out = order;
            return true;
        }
    }
    return false;
}

inline bool transformOrderFrom(std::string_view key, TransformOrder& out) {
    for (const TransformOrder order : {TransformOrder::SRT, TransformOrder::STR, TransformOrder::RST,
                                       TransformOrder::RTS, TransformOrder::TSR, TransformOrder::TRS}) {
        if (sameOrderKey(key, toString(order))) {
            out = order;
            return true;
        }
    }
    return false;
}

inline std::array<int, 3> axesOf(RotationOrder order) {
    switch (order) {
        case RotationOrder::XYZ: return {0, 1, 2};
        case RotationOrder::XZY: return {0, 2, 1};
        case RotationOrder::YXZ: return {1, 0, 2};
        case RotationOrder::YZX: return {1, 2, 0};
        case RotationOrder::ZXY: return {2, 0, 1};
        case RotationOrder::ZYX: return {2, 1, 0};
    }
    return {2, 0, 1};
}

inline Mat4 rotationAbout(int axis, double degrees) {
    return axis == 0 ? rotationX(degrees) : axis == 1 ? rotationY(degrees) : rotationZ(degrees);
}

inline double component(const Vec3& v, int axis) {
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}

/// The rotation `degrees` composed in `order`.
inline Mat4 rotation(const Vec3& degrees, RotationOrder order) {
    const std::array<int, 3> axes = axesOf(order);
    return rotationAbout(axes[0], component(degrees, axes[0])) *
           rotationAbout(axes[1], component(degrees, axes[1])) *
           rotationAbout(axes[2], component(degrees, axes[2]));
}

/// Angles back out of a rotation, in `order`. The inverse of `rotation` away
/// from gimbal lock; at the lock the middle angle is ±90 and the other two are
/// split arbitrarily, as every decomposition must.
///
/// One rule for all six orders rather than six hand-written cases: for
/// `R = R_a * R_b * R_c`, the element at row a, column c is sin(b) for a
/// cyclic order (XYZ, YZX, ZXY) and -sin(b) for the other three, and the
/// outer angles follow from the same row and column.
inline Vec3 eulerFromRotation(const Mat4& r, RotationOrder order) {
    const std::array<int, 3> ax = axesOf(order);
    const int a = ax[0];
    const int b = ax[1];
    const int c = ax[2];
    const bool cyclic = (a == 0 && b == 1) || (a == 1 && b == 2) || (a == 2 && b == 0);
    const double sign = cyclic ? 1.0 : -1.0;
    const double sb = std::fmin(1.0, std::fmax(-1.0, sign * r.at(a, c)));
    const double beta = std::asin(sb);
    double alpha;
    double gamma;
    if (std::fabs(sb) < 1.0 - 1e-9) {
        alpha = std::atan2(-sign * r.at(b, c), r.at(c, c));
        gamma = std::atan2(-sign * r.at(a, b), r.at(a, a));
    } else {
        // Gimbal lock: give everything to the first angle.
        alpha = std::atan2(sign * r.at(b, a), r.at(b, b));
        gamma = 0.0;
    }
    Vec3 out;
    (a == 0 ? out.x : a == 1 ? out.y : out.z) = alpha * kToDegrees;
    (b == 0 ? out.x : b == 1 ? out.y : out.z) = beta * kToDegrees;
    (c == 0 ? out.x : c == 1 ? out.y : out.z) = gamma * kToDegrees;
    return out;
}

/// Everything Nuke's Axis holds, in Nuke's units.
struct Transform {
    Vec3           translate;
    Vec3           rotate;                 ///< degrees
    Vec3           scale{1.0, 1.0, 1.0};
    double         uniformScale = 1.0;
    Vec3           skew;
    Vec3           pivot;
    RotationOrder  rotationOrder = RotationOrder::ZXY;
    TransformOrder transformOrder = TransformOrder::SRT;
};

/// The local matrix: the three factors composed in the order the transform
/// order says they are applied, rotation and scale each about the pivot.
///
/// Conjugating R and K*S by the pivot separately, rather than the product,
/// is what makes every order come out as Nuke documents it: for SRT the two
/// inner pivot translations cancel and the result is T(t) T(p) R K S T(-p).
inline Mat4 localMatrix(const Transform& t) {
    const Mat4 toPivot = translation(t.pivot);
    const Mat4 fromPivot = translation(t.pivot * -1.0);
    const Vec3 s{t.scale.x * t.uniformScale, t.scale.y * t.uniformScale,
                 t.scale.z * t.uniformScale};
    const Mat4 factorS = toPivot * skewing(t.skew) * scaling(s) * fromPivot;
    const Mat4 factorR = toPivot * rotation(t.rotate, t.rotationOrder) * fromPivot;
    const Mat4 factorT = translation(t.translate);
    const auto pick = [&](char which) -> const Mat4& {
        return which == 'S' ? factorS : which == 'R' ? factorR : factorT;
    };
    const char* order = toString(t.transformOrder);
    // Applied first is rightmost in the product.
    return pick(order[2]) * pick(order[1]) * pick(order[0]);
}

/// The inverse of an affine matrix with any 3x3 -- scale and skew included --
/// by cofactors. A rigid transform inverts with a transpose, but a camera
/// under a scaled parent is not rigid.
inline Mat4 inverseAffine(const Mat4& m) {
    const double a00 = m.at(0, 0), a01 = m.at(0, 1), a02 = m.at(0, 2);
    const double a10 = m.at(1, 0), a11 = m.at(1, 1), a12 = m.at(1, 2);
    const double a20 = m.at(2, 0), a21 = m.at(2, 1), a22 = m.at(2, 2);
    const double c00 = a11 * a22 - a12 * a21;
    const double c01 = a12 * a20 - a10 * a22;
    const double c02 = a10 * a21 - a11 * a20;
    const double det = a00 * c00 + a01 * c01 + a02 * c02;
    const double inv = std::fabs(det) > 1e-18 ? 1.0 / det : 0.0;
    Mat4 out;
    out.at(0, 0) = c00 * inv;
    out.at(0, 1) = (a02 * a21 - a01 * a22) * inv;
    out.at(0, 2) = (a01 * a12 - a02 * a11) * inv;
    out.at(1, 0) = c01 * inv;
    out.at(1, 1) = (a00 * a22 - a02 * a20) * inv;
    out.at(1, 2) = (a02 * a10 - a00 * a12) * inv;
    out.at(2, 0) = c02 * inv;
    out.at(2, 1) = (a01 * a20 - a00 * a21) * inv;
    out.at(2, 2) = (a00 * a11 - a01 * a10) * inv;
    const Vec3 t = m.translation();
    const Vec3 back = out.direction(t) * -1.0;
    out.at(0, 3) = back.x;
    out.at(1, 3) = back.y;
    out.at(2, 3) = back.z;
    return out;
}

/// The determinant of the upper 3x3: negative means a mirror.
inline double determinant3(const Mat4& m) {
    return m.at(0, 0) * (m.at(1, 1) * m.at(2, 2) - m.at(1, 2) * m.at(2, 1)) -
           m.at(0, 1) * (m.at(1, 0) * m.at(2, 2) - m.at(1, 2) * m.at(2, 0)) +
           m.at(0, 2) * (m.at(1, 0) * m.at(2, 1) - m.at(1, 1) * m.at(2, 0));
}

/// An object at `eye`, turned so that its local `lookAxis` points at
/// `target`, with `up` deciding the roll. Returns object-to-world. Nuke's
/// look axis is -Z, and a camera's is -Z: `lookAt(eye, target)` with the
/// default is a camera looking at `target`.
///
/// Always a proper rotation: right = up × back for a -Z look, right =
/// up × forward for a +Z one, and the third axis completes a right-handed
/// frame either way.
inline Mat4 lookAt(const Vec3& eye, const Vec3& target, const Vec3& up = {0.0, 1.0, 0.0},
                   const Vec3& lookAxis = {0.0, 0.0, -1.0}) {
    const Vec3 f = normalised(target - eye);
    if (length(f) < 1e-12) {
        return translation(eye);
    }
    // The frame that has +Z where the look axis points, then the look axis's
    // own frame is that turned so that `lookAxis` lands on the look direction.
    const bool lookingDownMinusZ = lookAxis.z < 0.0;
    const Vec3 zAxis = lookingDownMinusZ ? f * -1.0 : f;
    Vec3 xAxis = normalised(cross(up, zAxis));
    if (length(xAxis) < 1e-12) {
        // Looking straight up or down: any horizontal right will do.
        xAxis = normalised(cross(Vec3{0.0, 0.0, 1.0}, zAxis));
        if (length(xAxis) < 1e-12) {
            xAxis = {1.0, 0.0, 0.0};
        }
    }
    const Vec3 yAxis = cross(zAxis, xAxis);
    Mat4 out;
    out.at(0, 0) = xAxis.x; out.at(0, 1) = yAxis.x; out.at(0, 2) = zAxis.x; out.at(0, 3) = eye.x;
    out.at(1, 0) = xAxis.y; out.at(1, 1) = yAxis.y; out.at(1, 2) = zAxis.y; out.at(1, 3) = eye.y;
    out.at(2, 0) = xAxis.z; out.at(2, 1) = yAxis.z; out.at(2, 2) = zAxis.z; out.at(2, 3) = eye.z;
    return out;
}

/// The rotation in an affine matrix: its three axes, each brought back to
/// unit length, so scale drops out and what is left decomposes to angles. A
/// skewed matrix gives the nearest thing to a rotation its first axis allows;
/// nothing here re-orthogonalises, because a camera is not skewed.
inline Mat4 rotationPart(const Mat4& m) {
    Mat4 out;
    for (int c = 0; c < 3; ++c) {
        const Vec3 axis = normalised(m.column(c));
        out.at(0, c) = axis.x;
        out.at(1, c) = axis.y;
        out.at(2, c) = axis.z;
    }
    return out;
}

/// World to view, from a camera's object-to-world.
inline Mat4 viewFromCameraWorld(const Mat4& cameraToWorld) {
    return inverseAffine(cameraToWorld);
}

}   // namespace aofx::xform
