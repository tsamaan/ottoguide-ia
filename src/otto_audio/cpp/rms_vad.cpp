#include "rms_vad.hpp"
#include <cmath>

bool RmsVad::is_speech(const int16_t* window, size_t n) {
    if (n == 0) return false;
    double sum = 0;
    for (size_t i = 0; i < n; ++i)
        sum += (double)window[i] * window[i];
    float rms = (float)std::sqrt(sum / (double)n);
    return rms >= threshold_;
}
