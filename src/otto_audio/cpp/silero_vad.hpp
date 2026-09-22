#pragma once
#include "vad.hpp"
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

// Envoltorio de Silero VAD sobre ONNX Runtime, implementa
// VoiceActivityDetector. `pimpl` a propósito: los headers de onnxruntime
// no se filtran a quien solo necesita is_speech()/speech_probability()
// (otto_pipeline.cpp, vad_harness.cpp).
class SileroVad : public VoiceActivityDetector {
public:
    // threshold: umbral de probabilidad [0,1] que usa is_speech() (no
    // afecta a speech_probability(), que siempre devuelve el valor crudo
    // del modelo -- útil para el harness, que prueba varios umbrales).
    explicit SileroVad(const std::string& model_path, float threshold = 0.5f);
    ~SileroVad();

    SileroVad(const SileroVad&) = delete;
    SileroVad& operator=(const SileroVad&) = delete;

    // window debe tener exactamente window_size() muestras int16 mono a
    // 16kHz. Devuelve la probabilidad de voz [0,1] de esa ventana. El
    // modelo es stateful (arrastra contexto entre llamadas) -- llamar en
    // orden sobre audio continuo, o usar reset() al empezar un audio nuevo.
    float speech_probability(const int16_t* window, size_t n);

    bool is_speech(const int16_t* window, size_t n) override {
        return speech_probability(window, n) >= threshold_;
    }

    int window_size() const override { return 512; } // fijo para Silero a 16kHz

    void reset() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    float threshold_;
};
