#include <cmath>

extern "C" {
    float sword_math_abs_f32(float number){
        return abs(number);
    }
    float sword_math_acos_f32(float number){
        return acos(number);
    }
    float sword_math_acosh_f32(float number){
        return acosh(number)
    }
}

extern "C" {
    double sword_math_abs_f64(double number){
        return abs(number);
    }
}