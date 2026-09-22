#pragma once
#include <cstdint>
#include <cstddef>

// Interfaz para detección de voz — la máquina de estados de otto_pipeline
// (tomar_utterance) depende SOLO de esto, nunca de RmsVad/SileroVad
// directamente. Permite cambiar de implementación (o caer a un respaldo
// si Silero no carga) sin tocar la lógica de captura.
class VoiceActivityDetector {
public:
    virtual ~VoiceActivityDetector() = default;

    // window debe tener exactamente window_size() muestras, 16kHz mono.
    virtual bool is_speech(const int16_t* window, size_t n) = 0;

    // Tamaño de ventana que esta implementación necesita (en muestras).
    // Cada implementación puede pedir el suyo -- tomar_utterance() se
    // adapta, no asume un tamaño fijo.
    virtual int window_size() const = 0;

    // Limpia cualquier estado interno (usar al empezar una utterance nueva).
    virtual void reset() = 0;
};
