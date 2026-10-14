#include <cmath>

extern "C" {
#include <cmath>

extern "C" {
    float sword_math_abs_f32(float x) {
        return abs(x);
    }
    float sword_math_acos_f32(float x) {
        return acos(x);
    }
    float sword_math_acosh_f32(float x) {
        return acosh(x);
    }
    float sword_math_asin_f32(float x) {
        return asin(x);
    }
    float sword_math_asinh_f32(float x) {
        return asinh(x);
    }
    float sword_math_atan_f32(float x) {
        return atan(x);
    }
    float sword_math_atan2_f32(float y, float x) {
        return atan2f(y, x);
    }
    float sword_math_atanh_f32(float x) {
        return atanhf(x);
    }

    float sword_math_cbrt_f32(float x) {
        return cbrtf(x);
    }

    float sword_math_ceil_f32(float x) {
        return ceilf(x);
    }

    float sword_math_copysign_f32(float x, float y) {
        return copysignf(x, y);
    }

    float sword_math_cos_f32(float x) {
        return cosf(x);
    }

    float sword_math_cosh_f32(float x) {
        return coshf(x);
    }

    float sword_math_exp_f32(float x) {
        return expf(x);
    }

    float sword_math_exp2_f32(float x) {
        return exp2f(x);
    }

    float sword_math_expm1_f32(float x) {
        return expm1f(x);
    }

    float sword_math_erf_f32(float x) {
        return erff(x);
    }

    float sword_math_erfc_f32(float x) {
        return erfcf(x);
    }

    float sword_math_fdim_f32(float x, float y) {
        return fdimf(x, y);
    }

    float sword_math_floor_f32(float x) {
        return floorf(x);
    }

    float sword_math_fma_f32(float x, float y, float z) {
        return fmaf(x, y, z);
    }

    float sword_math_fmax_f32(float x, float y) {
        return fmaxf(x, y);
    }

    float sword_math_fmin_f32(float x, float y) {
        return fminf(x, y);
    }

    float sword_math_fmod_f32(float x, float y) {
        return fmodf(x, y);
    }

    float sword_math_frexp_f32(float x, float y) {
        int exp = 0;
        return frexpf(x, &exp);
    }

    float sword_math_hypot_f32(float x, float y) {
        return hypotf(x, y);
    }

    int sword_math_ilogb_f32(float x) {
        return ilogbf(x);
    }

    float sword_math_ldexp_f32(float x, float y) {
        return ldexpf(x, static_cast<int>(y));
    }

    float sword_math_lgamma_f32(float x) {
        return lgammaf(x);
    }

    long long sword_math_llrint_f32(float x) {
        return llrintf(x);
    }

    long long sword_math_llround_f32(float x) {
        return llroundf(x);
    }

    float sword_math_log_f32(float x) {
        return logf(x);
    }

    float sword_math_log10_f32(float x) {
        return log10f(x);
    }

    float sword_math_log2_f32(float x) {
        return log2f(x);
    }

    float sword_math_logb_f32(float x) {
        return logbf(x);
    }

    long sword_math_lrint_f32(float x) {
        return lrintf(x);
    }

    long sword_math_lround_f32(float x) {
        return lroundf(x);
    }

    float sword_math_nearbyint_f32(float x) {
        return nearbyintf(x);
    }

    float sword_math_nextafter_f32(float x, float y) {
        return nextafterf(x, y);
    }

    float sword_math_nexttoward_f32(float x, float y) {
        return nexttowardf(x, static_cast<long double>(y));
    }

    float sword_math_pow_f32(float x, float y) {
        return powf(x, y);
    }

    float sword_math_remainder_f32(float x, float y) {
        return remainderf(x, y);
    }

    float sword_math_remquo_f32(float x, float y, float z) {
        int quo = 0;
        return remquof(x, y, &quo);
    }

    int sword_math_rint_f32(float x) {
        return static_cast<int>(rintf(x));
    }

    // Note: The original list contained a duplicate declaration for sword_math_lround_f32 
    // returning int instead of long. Since C linkage does not allow function overloading 
    // by return type alone, only the long version is kept above.

    float sword_math_scalbln_f32(float x, float y) {
        return scalblnf(x, static_cast<long>(y));
    }

    float sword_math_scalbn_f32(float x, float y) {
        return scalbnf(x, static_cast<int>(y));
    }

    float sword_math_sin_f32(float x) {
        return sinf(x);
    }

    float sword_math_sinh_f32(float x) {
        return sinhf(x);
    }

    float sword_math_sqrt_f32(float x) {
        return sqrtf(x);
    }

    float sword_math_tan_f32(float x) {
        return tanf(x);
    }

    float sword_math_tanh_f32(float x) {
        return tanhf(x);
    }

    float sword_math_tgamma_f32(float x) {
        return tgammaf(x);
    }

    float sword_math_trunc_f32(float x) {
        return truncf(x);
    }

    }
}

extern "C" {
    double sword_math_abs_f64(double x){
        return abs(x);
    }
}