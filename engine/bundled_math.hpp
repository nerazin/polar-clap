#pragma once
// The math library statically linked into the original Polar binary, ported to C++.
//
// The original carries its own libm (only memset is imported). Its routines are
// newlib's fdlibm float/double code: thresholds, polynomial coefficients and
// reduction tables were read from the x86-64 slice (SHA-256 94b6c8dd...f11e) and
// match newlib, including newlib's powf t_h constant (+0x40000). Platform libms
// differ from it in ~10-14% of expf/powf/atan2f/cosf results, which is why the
// port calls these instead of <cmath>. Proven against the original by
// tests/bundled_math_check (see README, "Accuracy").
//
// Every operation is plain IEEE binary32/binary64; build without fast-math and
// with -ffp-contract=off so results are identical on x86-64 and arm64.
//
// fdlibm: Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
// Developed at SunPro, a Sun Microsystems, Inc. business. Permission to use,
// copy, modify, and distribute this software is freely granted, provided that
// this notice is preserved. Float conversion by Ian Lance Taylor, Cygnus Support.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace polar_recovered {
namespace bundled {
namespace detail {

inline std::int32_t word(float x) { std::int32_t w; std::memcpy(&w, &x, 4); return w; }
inline float fromWord(std::int32_t w) { float x; std::memcpy(&x, &w, 4); return x; }
inline float fromWord(std::uint32_t w) { float x; std::memcpy(&x, &w, 4); return x; }
inline std::int32_t high(double x) { std::uint64_t b; std::memcpy(&b, &x, 8); return std::int32_t(b >> 32); }
inline double fromHighLow(std::uint32_t h, std::uint32_t l) {
    const std::uint64_t b = (std::uint64_t(h) << 32) | l; double x; std::memcpy(&x, &b, 8); return x;
}
inline float f(std::uint32_t w) { return fromWord(w); }  // constant from its bit pattern
// Exponent arithmetic on words; the C original shifts negative ints, which is UB in C++.
inline std::int32_t addExponent(std::int32_t w, std::int32_t k) { return std::int32_t(std::uint32_t(w) + (std::uint32_t(k) << 23)); }

// ---- binary32 kernels (kf_sin.c, kf_cos.c), |x| <= pi/4
inline float kernelSinf(float x, float y, int iy) {
    const float S1 = f(0xbe2aaaab), S2 = f(0x3c088889), S3 = f(0xb9500d01),
                S4 = f(0x3638ef1b), S5 = f(0xb2d72f34), S6 = f(0x2f2ec9d3);
    const std::int32_t ix = word(x) & 0x7fffffff;
    if (ix < 0x32000000 && int(x) == 0) return x;
    const float z = x * x, v = z * x, r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0) return x + v * (S1 + z * r);
    return x - ((z * (0.5f * y - v * r) - y) - v * S1);
}
inline float kernelCosf(float x, float y) {
    const float C1 = f(0x3d2aaaab), C2 = f(0xbab60b61), C3 = f(0x37d00d01),
                C4 = f(0xb493f27c), C5 = f(0x310f74f6), C6 = f(0xad47d74e);
    const std::int32_t ix = word(x) & 0x7fffffff;
    if (ix < 0x32000000 && int(x) == 0) return 1.0f;
    const float z = x * x, r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ix < 0x3e99999a) return 1.0f - (0.5f * z - (z * r - x * y));
    const float qx = ix > 0x3f480000 ? 0.28125f : fromWord(ix - 0x01000000);
    const float hz = 0.5f * z - qx, a = 1.0f - qx;
    return a - (hz - (z * r - x * y));
}

// ---- ef_rem_pio2.c. Returns false outside |x| <= 2^7*pi/2 (the large-argument
// path, __kernel_rem_pio2f, is not ported: Polar only evaluates angles in [0, 2pi]).
inline bool remPio2f(float x, float* y, std::int32_t* n) {
    static const std::int32_t npio2_hw[32] = {
        0x3fc90f00, 0x40490f00, 0x4096cb00, 0x40c90f00, 0x40fb5300, 0x4116cb00, 0x412fed00, 0x41490f00,
        0x41623100, 0x417b5300, 0x418a3a00, 0x4196cb00, 0x41a35c00, 0x41afed00, 0x41bc7e00, 0x41c90f00,
        0x41d5a000, 0x41e23100, 0x41eec200, 0x41fb5300, 0x4203f200, 0x420a3a00, 0x42108300, 0x4216cb00,
        0x421d1400, 0x42235c00, 0x4229a500, 0x422fed00, 0x42363600, 0x423c7e00, 0x4242c700, 0x42490f00};
    const float invpio2 = f(0x3f22f984), pio2_1 = f(0x3fc90f80), pio2_1t = f(0x37354443),
                pio2_2 = f(0x37354400), pio2_2t = f(0x2e85a308), pio2_3 = f(0x2e85a300),
                pio2_3t = f(0x248d3132);
    const std::int32_t hx = word(x), ix = hx & 0x7fffffff;
    if (ix <= 0x3f490fd8) { y[0] = x; y[1] = 0; *n = 0; return true; }
    if (ix < 0x4016cbe4) {  // |x| < 3pi/4, n = +-1
        if (hx > 0) {
            float z = x - pio2_1;
            if ((ix & 0xfffffff0) != 0x3fc90fd0) { y[0] = z - pio2_1t; y[1] = (z - y[0]) - pio2_1t; }
            else { z -= pio2_2; y[0] = z - pio2_2t; y[1] = (z - y[0]) - pio2_2t; }
            *n = 1;
        } else {
            float z = x + pio2_1;
            if ((ix & 0xfffffff0) != 0x3fc90fd0) { y[0] = z + pio2_1t; y[1] = (z - y[0]) + pio2_1t; }
            else { z += pio2_2; y[0] = z + pio2_2t; y[1] = (z - y[0]) + pio2_2t; }
            *n = -1;
        }
        return true;
    }
    if (ix > 0x43490f80) return false;
    float t = std::fabs(x);
    const std::int32_t m = std::int32_t(t * invpio2 + 0.5f);
    const float fn = float(m);
    float r = t - fn * pio2_1, w = fn * pio2_1t;
    if (m < 32 && std::int32_t(ix & 0xffffff00) != npio2_hw[m - 1]) {
        y[0] = r - w;
    } else {
        const std::int32_t j = ix >> 23;
        y[0] = r - w;
        std::int32_t i = j - ((word(y[0]) >> 23) & 0xff);
        if (i > 8) {
            t = r; w = fn * pio2_2; r = t - w; w = fn * pio2_2t - ((t - r) - w); y[0] = r - w;
            i = j - ((word(y[0]) >> 23) & 0xff);
            if (i > 25) { t = r; w = fn * pio2_3; r = t - w; w = fn * pio2_3t - ((t - r) - w); y[0] = r - w; }
        }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) { y[0] = -y[0]; y[1] = -y[1]; *n = -m; } else *n = m;
    return true;
}

// ---- binary64 kernels (k_sin.c, k_cos.c) and e_rem_pio2.c medium path
inline double kernelSin(double x, double y, int iy) {
    const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
                 S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
                 S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
    const std::int32_t ix = high(x) & 0x7fffffff;
    if (ix < 0x3e400000 && int(x) == 0) return x;
    const double z = x * x, v = z * x, r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0) return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}
inline double kernelCos(double x, double y) {
    const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
                 C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
                 C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    const std::int32_t ix = high(x) & 0x7fffffff;
    if (ix < 0x3e400000 && int(x) == 0) return 1.0;
    const double z = x * x, r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ix < 0x3fd33333) return 1.0 - (0.5 * z - (z * r - x * y));
    const double qx = ix > 0x3fe90000 ? 0.28125 : fromHighLow(std::uint32_t(ix - 0x00200000), 0);
    const double hz = 0.5 * z - qx, a = 1.0 - qx;
    return a - (hz - (z * r - x * y));
}
// Returns false outside |x| <= 2^19*pi/2 (large-argument path not ported, never reached).
inline bool remPio2(double x, double* y, std::int32_t* n) {
    static const std::int32_t npio2_hw[32] = {
        0x3ff921fb, 0x400921fb, 0x4012d97c, 0x401921fb, 0x401f6a7a, 0x4022d97c, 0x4025fdbb, 0x402921fb,
        0x402c463a, 0x402f6a7a, 0x4031475c, 0x4032d97c, 0x40346b9c, 0x4035fdbb, 0x40378fdb, 0x403921fb,
        0x403ab41b, 0x403c463a, 0x403dd85a, 0x403f6a7a, 0x40407e4c, 0x4041475c, 0x4042106c, 0x4042d97c,
        0x4043a28c, 0x40446b9c, 0x404534ac, 0x4045fdbb, 0x4046c6cb, 0x40478fdb, 0x404858eb, 0x404921fb};
    const double invpio2 = 6.36619772367581382433e-01, pio2_1 = 1.57079632673412561417e+00,
                 pio2_1t = 6.07710050650619224932e-11, pio2_2 = 6.07710050630396597660e-11,
                 pio2_2t = 2.02226624879595063154e-21, pio2_3 = 2.02226624871116645580e-21,
                 pio2_3t = 8.47842766036889956997e-32;
    const std::int32_t hx = high(x), ix = hx & 0x7fffffff;
    if (ix <= 0x3fe921fb) { y[0] = x; y[1] = 0; *n = 0; return true; }
    if (ix < 0x4002d97c) {
        if (hx > 0) {
            double z = x - pio2_1;
            if (ix != 0x3ff921fb) { y[0] = z - pio2_1t; y[1] = (z - y[0]) - pio2_1t; }
            else { z -= pio2_2; y[0] = z - pio2_2t; y[1] = (z - y[0]) - pio2_2t; }
            *n = 1;
        } else {
            double z = x + pio2_1;
            if (ix != 0x3ff921fb) { y[0] = z + pio2_1t; y[1] = (z - y[0]) + pio2_1t; }
            else { z += pio2_2; y[0] = z + pio2_2t; y[1] = (z - y[0]) + pio2_2t; }
            *n = -1;
        }
        return true;
    }
    if (ix > 0x413921fb) return false;
    double t = std::fabs(x);
    const std::int32_t m = std::int32_t(t * invpio2 + 0.5);
    const double fn = double(m);
    double r = t - fn * pio2_1, w = fn * pio2_1t;
    if (m < 32 && ix != npio2_hw[m - 1]) {
        y[0] = r - w;
    } else {
        const std::int32_t j = ix >> 20;
        y[0] = r - w;
        std::int32_t i = j - ((high(y[0]) >> 20) & 0x7ff);
        if (i > 16) {
            t = r; w = fn * pio2_2; r = t - w; w = fn * pio2_2t - ((t - r) - w); y[0] = r - w;
            i = j - ((high(y[0]) >> 20) & 0x7ff);
            if (i > 49) { t = r; w = fn * pio2_3; r = t - w; w = fn * pio2_3t - ((t - r) - w); y[0] = r - w; }
        }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) { y[0] = -y[0]; y[1] = -y[1]; *n = -m; } else *n = m;
    return true;
}

// ---- sf_atan.c (atan2f calls it; original 0x2ad20)
inline float atanf(float x) {
    static const std::uint32_t atanhi[4] = {0x3eed6338, 0x3f490fda, 0x3f7b985e, 0x3fc90fda};
    static const std::uint32_t atanlo[4] = {0x31ac3769, 0x33222168, 0x33140fb4, 0x33a22168};
    static const std::uint32_t aT[11] = {0x3eaaaaab, 0xbe4ccccd, 0x3e124925, 0xbde38e38, 0x3dba2e6e, 0xbd9d8795,
                                         0x3d886b35, 0xbd6ef16b, 0x3d4bda59, 0xbd15a221, 0x3c8569d7};
    const std::int32_t hx = word(x), ix = hx & 0x7fffffff;
    int id;
    if (ix >= 0x50800000) {  // |x| >= 2^34
        if (ix > 0x7f800000) return x + x;
        return hx > 0 ? f(atanhi[3]) + f(atanlo[3]) : -f(atanhi[3]) - f(atanlo[3]);
    }
    if (ix < 0x3ee00000) {  // |x| < 0.4375
        if (ix < 0x31000000) return x;  // |x| < 2^-29 (raises inexact in the original; same value)
        id = -1;
    } else {
        x = std::fabs(x);
        if (ix < 0x3f980000) {
            if (ix < 0x3f300000) { id = 0; x = (2.0f * x - 1.0f) / (2.0f + x); }
            else { id = 1; x = (x - 1.0f) / (x + 1.0f); }
        } else {
            if (ix < 0x401c0000) { id = 2; x = (x - 1.5f) / (1.0f + 1.5f * x); }
            else { id = 3; x = -1.0f / x; }
        }
    }
    const float z = x * x, w = z * z;
    const float s1 = z * (f(aT[0]) + w * (f(aT[2]) + w * (f(aT[4]) + w * (f(aT[6]) + w * (f(aT[8]) + w * f(aT[10]))))));
    const float s2 = w * (f(aT[1]) + w * (f(aT[3]) + w * (f(aT[5]) + w * (f(aT[7]) + w * f(aT[9])))));
    if (id < 0) return x - x * (s1 + s2);
    const float r = f(atanhi[id]) - ((x * (s1 + s2) - f(atanlo[id])) - x);
    return hx < 0 ? -r : r;
}

}  // namespace detail

namespace detail {
// ---- ef_exp.c core (original 0x2d440)
inline float expfCore(float x) {
    const float halF[2] = {0.5f, -0.5f}, ln2HI[2] = {f(0x3f317180), f(0xbf317180)},
                ln2LO[2] = {f(0x3717f7d1), f(0xb717f7d1)}, invln2 = f(0x3fb8aa3b),
                P1 = f(0x3e2aaaab), P2 = f(0xbb360b61), P3 = f(0x388ab355), P4 = f(0xb5ddea0e),
                P5 = f(0x3331bb4c), twom100 = f(0x0d800000);
    const std::int32_t sx = word(x), xsb = (sx >> 31) & 1, hx = sx & 0x7fffffff;
    if (hx > 0x7f800000) return x + x;
    if (hx == 0x7f800000) return xsb == 0 ? x : 0.0f;
    if (sx > 0x42b17217) return f(0x7f800000);  // overflow: huge*huge
    if (sx < 0 && hx > 0x42cff1b5) return 0.0f;  // underflow: twom100*twom100
    float hi = 0, lo = 0;
    std::int32_t k = 0;
    if (hx > 0x3eb17218) {  // |x| > 0.5 ln2
        if (hx < 0x3f851592) { hi = x - ln2HI[xsb]; lo = ln2LO[xsb]; k = 1 - xsb - xsb; }
        else {
            k = std::int32_t(invln2 * x + halF[xsb]);
            const float t = float(k);
            hi = x - t * ln2HI[0];
            lo = t * ln2LO[0];
        }
        x = hi - lo;
    } else if (hx < 0x31800000) {  // |x| < 2^-28
        return 1.0f + x;
    }
    const float t = x * x;
    const float c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    if (k == 0) return 1.0f - ((x * c) / (c - 2.0f) - x);
    const float y = 1.0f - ((lo - (x * c) / (2.0f - c)) - hi);
    if (k >= -125) return fromWord(addExponent(word(y), k));
    return fromWord(addExponent(word(y), k + 100)) * twom100;
}
}  // namespace detail

// ---- wf_exp.c wrapper (original 0x2af10). The binary's _LIB_VERSION is 1 (_XOPEN_), so
// finite inputs beyond its thresholds return inf / 0 before the core's own limits apply.
inline float expf(float x) {
    using namespace detail;
    const float r = expfCore(x);
    if ((word(x) & 0x7f800000) != 0x7f800000) {
        if (x > f(0x42b17180)) return f(0x7f800000);
        if (x < f(0xc2cff1b5)) return 0.0f;
    }
    return r;
}

// ---- ef_pow.c (original 0x2dc40). shortcut: the wrapper 0x2b0f0 is not ported; it only
// rewrites error results (0**negative, negative**non-integer, overflow) that Polar never
// produces (it calls powf(128, t) with t in [0, 1]). Port it before using powf elsewhere.
inline float powf(float x, float y) {
    using namespace detail;
    const float bp[2] = {1.0f, 1.5f}, dp_h[2] = {0.0f, f(0x3f15c000)}, dp_l[2] = {0.0f, f(0x35d1cfdc)};
    const float two24 = f(0x4b800000), huge = f(0x7149f2ca), tiny = f(0x0da24260);
    const float L1 = f(0x3f19999a), L2 = f(0x3edb6db7), L3 = f(0x3eaaaaab), L4 = f(0x3e8ba305),
                L5 = f(0x3e6c3255), L6 = f(0x3e53f142);
    const float P1 = f(0x3e2aaaab), P2 = f(0xbb360b61), P3 = f(0x388ab355), P4 = f(0xb5ddea0e), P5 = f(0x3331bb4c);
    const float lg2 = f(0x3f317218), lg2_h = f(0x3f317200), lg2_l = f(0x35bfbe8c), ovt = f(0x3338aa3c);
    const float cp = f(0x3f76384f), cp_h = f(0x3f763800), cp_l = f(0x369dc3a0);
    const float ivln2 = f(0x3fb8aa3b), ivln2_h = f(0x3fb8aa00), ivln2_l = f(0x36eca570);
    const std::int32_t hx = word(x), hy = word(y), ix = hx & 0x7fffffff, iy = hy & 0x7fffffff;
    float z, ax, z_h, z_l, p_h, p_l, y1, t1, t2, r, s, t, u, v, w;
    std::int32_t i, j, k, yisint, n;

    if (iy == 0) return 1.0f;
    if (ix > 0x7f800000 || iy > 0x7f800000) return ix == 0x3f800000 ? 1.0f : f(0x7fc00000);
    yisint = 0;
    if (hx < 0) {
        if (iy >= 0x4b800000) yisint = 2;
        else if (iy >= 0x3f800000) {
            k = (iy >> 23) - 0x7f;
            j = iy >> (23 - k);
            if ((j << (23 - k)) == iy) yisint = 2 - (j & 1);
        }
    }
    if (iy == 0x7f800000) {
        if (ix == 0x3f800000) return 1.0f;
        if (ix > 0x3f800000) return hy >= 0 ? y : 0.0f;
        return hy < 0 ? -y : 0.0f;
    }
    if (iy == 0x3f800000) return hy < 0 ? 1.0f / x : x;
    if (hy == 0x40000000) return x * x;
    if (hy == 0x3f000000 && hx >= 0) return std::sqrt(x);  // IEEE sqrt: bit-exact with the bundled one
    ax = std::fabs(x);
    if (ix == 0x7f800000 || ix == 0 || ix == 0x3f800000) {
        z = ax;
        if (hy < 0) z = 1.0f / z;
        if (hx < 0) {
            if (((ix - 0x3f800000) | yisint) == 0) z = (z - z) / (z - z);
            else if (yisint == 1) z = -z;
        }
        return z;
    }
    if (((std::uint32_t(hx) >> 31) - 1 | std::uint32_t(yisint)) == 0) return (x - x) / (x - x);
    if (iy > 0x4d000000) {  // |y| > 2^27
        if (ix < 0x3f7ffff8) return hy < 0 ? huge * huge : tiny * tiny;
        if (ix > 0x3f800007) return hy > 0 ? huge * huge : tiny * tiny;
        t = ax - 1.0f;
        w = (t * t) * (0.5f - t * (0.333333333333f - t * 0.25f));
        u = ivln2_h * t;
        v = t * ivln2_l - w * ivln2;
        t1 = fromWord(word(u + v) & std::int32_t(0xfffff000));
        t2 = v - (t1 - u);
    } else {
        float s2, s_h, s_l, t_h, t_l;
        std::int32_t ixn = ix;
        n = 0;
        if (ixn < 0x00800000) { ax *= two24; n -= 24; ixn = word(ax); }
        n += (ixn >> 23) - 0x7f;
        j = ixn & 0x007fffff;
        ixn = j | 0x3f800000;
        if (j <= 0x1cc471) k = 0;
        else if (j < 0x5db3d7) k = 1;
        else { k = 0; n += 1; ixn -= 0x00800000; }
        ax = fromWord(ixn);
        u = ax - bp[k];
        v = 1.0f / (ax + bp[k]);
        s = u * v;
        s_h = fromWord(word(s) & std::int32_t(0xfffff000));
        t_h = fromWord(((ixn >> 1) | 0x20000000) + 0x0040000 + (k << 21));  // newlib's constant, as in the binary
        t_l = ax - (t_h - bp[k]);
        s_l = v * ((u - s_h * t_h) - s_h * t_l);
        s2 = s * s;
        r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
        r += s_l * (s_h + s);
        s2 = s_h * s_h;
        t_h = fromWord(word(3.0f + s2 + r) & std::int32_t(0xfffff000));
        t_l = r - ((t_h - 3.0f) - s2);
        u = s_h * t_h;
        v = s_l * t_h + t_l * s;
        p_h = fromWord(word(u + v) & std::int32_t(0xfffff000));
        p_l = v - (p_h - u);
        z_h = cp_h * p_h;
        z_l = cp_l * p_h + p_l * cp + dp_l[k];
        t = float(n);
        t1 = fromWord(word(((z_h + z_l) + dp_h[k]) + t) & std::int32_t(0xfffff000));
        t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
    }
    s = 1.0f;
    if (((std::uint32_t(hx) >> 31) - 1 | std::uint32_t(yisint - 1)) == 0) s = -1.0f;
    y1 = fromWord(hy & std::int32_t(0xfffff000));
    p_l = (y - y1) * t1 + y * t2;
    p_h = y1 * t1;
    z = p_l + p_h;
    j = word(z);
    i = j & 0x7fffffff;
    if (j > 0) {
        if (i > 0x43000000) return s * huge * huge;
        if (i == 0x43000000 && p_l + ovt > z - p_h) return s * huge * huge;
    } else {
        if (i > 0x43160000) return s * tiny * tiny;
        if (i == 0x43160000 && p_l <= z - p_h) return s * tiny * tiny;
    }
    k = (i >> 23) - 0x7f;
    n = 0;
    if (i > 0x3f000000) {
        n = j + (0x00800000 >> (k + 1));
        k = ((n & 0x7fffffff) >> 23) - 0x7f;
        t = fromWord(n & ~(0x007fffff >> k));
        n = ((n & 0x007fffff) | 0x00800000) >> (23 - k);
        if (j < 0) n = -n;
        p_h -= t;
    }
    t = fromWord(word(p_l + p_h) & std::int32_t(0xfffff000));
    u = t * lg2_h;
    v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
    z = u + v;
    w = v - (z - u);
    t = z * z;
    t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    r = (z * t1) / (t1 - 2.0f) - (w + z * w);
    z = 1.0f - (r - z);
    j = addExponent(word(z), n);
    if ((j >> 23) <= 0) z = std::scalbn(z, int(n));  // subnormal result: exact, any libm
    else z = fromWord(j);
    return s * z;
}

// ---- ef_atan2.c (original 0x2cf50)
inline float atan2f(float y, float x) {
    using namespace detail;
    const float tiny = 1.0e-30f, pi_o_4 = f(0x3f490fdb), pi_o_2 = f(0x3fc90fdb), pi = f(0x40490fdb),
                pi_lo = f(0xb3bbbd2e);
    const std::int32_t hx = word(x), ix = hx & 0x7fffffff, hy = word(y), iy = hy & 0x7fffffff;
    if (ix > 0x7f800000 || iy > 0x7f800000) return x + y;
    if (hx == 0x3f800000) return atanf(y);
    const std::int32_t m = ((hy >> 31) & 1) | ((hx >> 30) & 2);
    if (iy == 0) {
        switch (m) { case 0: case 1: return y; case 2: return pi + tiny; default: return -pi - tiny; }
    }
    if (ix == 0) return hy < 0 ? -pi_o_2 - tiny : pi_o_2 + tiny;
    if (ix == 0x7f800000) {
        if (iy == 0x7f800000) {
            switch (m) { case 0: return pi_o_4 + tiny; case 1: return -pi_o_4 - tiny;
                         case 2: return 3.0f * pi_o_4 + tiny; default: return -3.0f * pi_o_4 - tiny; }
        }
        switch (m) { case 0: return 0.0f; case 1: return -0.0f; case 2: return pi + tiny; default: return -pi - tiny; }
    }
    if (iy == 0x7f800000) return hy < 0 ? -pi_o_2 - tiny : pi_o_2 + tiny;
    const std::int32_t k = (iy - ix) >> 23;
    float z;
    if (k > 60) z = pi_o_2 + 0.5f * pi_lo;
    else if (hx < 0 && k < -60) z = 0.0f;
    else z = atanf(std::fabs(y / x));
    switch (m) {
    case 0: return z;
    case 1: return fromWord(word(z) ^ std::int32_t(0x80000000));
    case 2: return pi - (z - pi_lo);
    default: return (z - pi_lo) - pi;
    }
}

// ---- e_exp.c + w_exp.c in binary64 (original 0x2aaf0 -> 0x2d230), used by the envelope
inline double exp(double x) {
    using namespace detail;
    const double halF[2] = {0.5, -0.5}, huge = 1.0e+300, twom1000 = 9.33263618503218878990e-302,
                 o_threshold = 7.09782712893383973096e+02, u_threshold = -7.45133219101941108420e+02,
                 ln2HI[2] = {6.93147180369123816490e-01, -6.93147180369123816490e-01},
                 ln2LO[2] = {1.90821492927058770002e-10, -1.90821492927058770002e-10},
                 invln2 = 1.44269504088896338700e+00, P1 = 1.66666666666666019037e-01,
                 P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
                 P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08;
    std::int32_t hx = high(x);
    const std::int32_t xsb = (hx >> 31) & 1;
    hx &= 0x7fffffff;
    if (hx >= 0x40862e42) {
        if (hx >= 0x7ff00000) return x != x ? x + x : (xsb == 0 ? x : 0.0);
        if (x > o_threshold) return huge * huge;
        if (x < u_threshold) return 0.0;  // twom1000*twom1000
    }
    double hi = 0, lo = 0;
    std::int32_t k = 0;
    if (hx > 0x3fd62e42) {
        if (hx < 0x3ff0a2b2) { hi = x - ln2HI[xsb]; lo = ln2LO[xsb]; k = 1 - xsb - xsb; }
        else {
            k = std::int32_t(invln2 * x + halF[xsb]);
            const double t = double(k);
            hi = x - t * ln2HI[0];
            lo = t * ln2LO[0];
        }
        x = hi - lo;
    } else if (hx < 0x3e300000) {
        return 1.0 + x;
    }
    const double t = x * x;
    const double c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    if (k == 0) return 1.0 - ((x * c) / (c - 2.0) - x);
    double y = 1.0 - ((lo - (x * c) / (2.0 - c)) - hi);
    std::uint64_t b; std::memcpy(&b, &y, 8);
    if (k >= -1021) {
        b += std::uint64_t(std::uint32_t(k)) << 52;
        std::memcpy(&y, &b, 8);
        return y;
    }
    b += std::uint64_t(std::uint32_t(k + 1000)) << 52;
    std::memcpy(&y, &b, 8);
    return y * twom1000;
}

// ---- sf_cos.c (original 0x2a6a0)
inline float cosf(float x) {
    using namespace detail;
    const std::int32_t ix = word(x) & 0x7fffffff;
    if (ix <= 0x3f490fd8) return kernelCosf(x, 0.0f);
    if (ix >= 0x7f800000) return x - x;
    float y[2]; std::int32_t n;
    // shortcut: |x| > 2^7*pi/2 falls back to the platform cosf; Polar never gets there.
    if (!remPio2f(x, y, &n)) return std::cos(x);
    switch (n & 3) {
    case 0: return kernelCosf(y[0], y[1]);
    case 1: return -kernelSinf(y[0], y[1], 1);
    case 2: return -kernelCosf(y[0], y[1]);
    default: return kernelSinf(y[0], y[1], 1);
    }
}

// ---- s_sin.c / s_cos.c in binary64 (original 0x2b440 / 0x2c110), used for the FFT0 trig table
inline double sin(double x) {
    using namespace detail;
    const std::int32_t ix = high(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) return kernelSin(x, 0.0, 0);
    if (ix >= 0x7ff00000) return x - x;
    double y[2]; std::int32_t n;
    if (!remPio2(x, y, &n)) return std::sin(x);  // shortcut: |x| > 2^19*pi/2, never reached
    switch (n & 3) {
    case 0: return kernelSin(y[0], y[1], 1);
    case 1: return kernelCos(y[0], y[1]);
    case 2: return -kernelSin(y[0], y[1], 1);
    default: return -kernelCos(y[0], y[1]);
    }
}
inline double cos(double x) {
    using namespace detail;
    const std::int32_t ix = high(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) return kernelCos(x, 0.0);
    if (ix >= 0x7ff00000) return x - x;
    double y[2]; std::int32_t n;
    if (!remPio2(x, y, &n)) return std::cos(x);  // shortcut: |x| > 2^19*pi/2, never reached
    switch (n & 3) {
    case 0: return kernelCos(y[0], y[1]);
    case 1: return -kernelSin(y[0], y[1], 1);
    case 2: return -kernelCos(y[0], y[1]);
    default: return kernelSin(y[0], y[1], 1);
    }
}

}  // namespace bundled
}  // namespace polar_recovered
