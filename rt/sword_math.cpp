#include <cmath>

extern "C" {
    float sword_math_trunc(float number){
        return trunc(number);
    }
}