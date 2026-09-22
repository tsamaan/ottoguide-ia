#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <string>

// Envoltorio de Silero VAD sobre ONNX Runtime. `pimpl` a propósito: los
// headers de onnxruntime no se filtran a quien solo necesita llamar
// speech_probability() (otto_pipeline.cpp, vad_harness.cpp).
class SileroVad {
public:
    explicit SileroVad(const std::string& model_path);
    ~SileroVad();

    SileroVad(const SileroVad&) = delete;
    SileroVad& operator=(const SileroVad&) = delete;

    // window debe tener exactamente window_size() muestras int16 mono a
    // 16kHz. Devuelve la probabilidad de voz [0,1] de esa ventana. El
    // modelo es stateful (arrastra contexto entre llamadas) -- llamar en
    // orden sobre audio continuo, o usar reset() al empezar un audio nuevo.
    float speech_probability(const int16_t* window, size_t n);

    int window_size() const { return 512; } // fijo para Silero a 16kHz

    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
