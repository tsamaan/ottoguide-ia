#pragma once
#include "vad.hpp"

// Respaldo: la lógica original de otto_pipeline (umbral de RMS sobre una
// ventana). Se usa si SileroVad no carga (modelo/librería faltante) — así
// el robot sigue funcionando, degradado pero no roto.
class RmsVad : public VoiceActivityDetector {
public:
    explicit RmsVad(float threshold, int window_samples = 2400 /* 150ms a 16kHz */)
        : threshold_(threshold), window_samples_(window_samples) {}

    bool is_speech(const int16_t* window, size_t n) override;
    int window_size() const override { return window_samples_; }
    void reset() override {} // sin estado interno

private:
    float threshold_;
    int window_samples_;
};
